#include "file_io.h"

#include <fcntl.h>
#include <limits.h>
#include <linux/kvm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_OPEN_FILES 64
#define MAX_NAME_LENGTH 255
#define MAX_TRANSFER_SIZE (8u * 1024u * 1024u)

#define GUEST_O_RD     1
#define GUEST_O_WR     2
#define GUEST_O_RDWR   4
#define GUEST_O_CREATE 8

#define GUEST_SEEK_SET 1
#define GUEST_SEEK_END 2

enum file_operation {
	FILE_OPEN = 1,
	FILE_CLOSE,
	FILE_READ,
	FILE_WRITE,
	FILE_LSEEK
};

struct open_file {
	int used;
	int host_fd;
	int flags;
	int shared_index;
};

struct file_protocol {
	char local_dir[PATH_MAX];
	const char **shared_paths;
	size_t shared_count;
	unsigned char *cow_done;
	struct open_file files[MAX_OPEN_FILES];

	uint32_t operation;
	uint32_t args[3];
	size_t expected_args;
	size_t arg_count;
	unsigned char *payload;
	size_t expected_payload;
	size_t payload_length;

	unsigned char *response;
	size_t response_length;
	size_t response_position;
};

static const char *file_name(const char *path)
{
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

static int valid_file_name(const char *name)
{
	if (!name[0] ||
	    !((name[0] >= 'A' && name[0] <= 'Z') ||
	      (name[0] >= 'a' && name[0] <= 'z')))
		return 0;

	for (size_t i = 1; name[i]; ++i) {
		if (!((name[i] >= 'A' && name[i] <= 'Z') ||
		      (name[i] >= 'a' && name[i] <= 'z') ||
		      (name[i] >= '0' && name[i] <= '9') ||
		      name[i] == '.'))
			return 0;
	}
	return 1;
}

static int guest_access_mode(int flags)
{
	int access = flags & (GUEST_O_RD | GUEST_O_WR | GUEST_O_RDWR);

	if (access == GUEST_O_RD)
		return O_RDONLY;
	if (access == GUEST_O_WR)
		return O_WRONLY;
	if (access == GUEST_O_RDWR)
		return O_RDWR;
	return -1;
}

static int local_path(struct file_protocol *protocol, const char *name,
		      char *path, size_t path_size)
{
	int length = snprintf(path, path_size, "%s/%s",
			      protocol->local_dir, name);
	return length >= 0 && (size_t)length < path_size ? 0 : -1;
}

static int find_shared(struct file_protocol *protocol, const char *name)
{
	for (size_t i = 0; i < protocol->shared_count; ++i) {
		if (!strcmp(file_name(protocol->shared_paths[i]), name))
			return (int)i;
	}
	return -1;
}

static int find_free_descriptor(struct file_protocol *protocol)
{
	for (int i = 0; i < MAX_OPEN_FILES; ++i) {
		if (!protocol->files[i].used)
			return i;
	}
	return -1;
}

static int open_guest_file(struct file_protocol *protocol, const char *name,
			   int flags)
{
	char path[PATH_MAX];
	int access_mode = guest_access_mode(flags);
	int descriptor = find_free_descriptor(protocol);
	int shared_index;
	int host_fd;

	if (!valid_file_name(name) || access_mode < 0 || descriptor < 0 ||
	    (flags & ~(GUEST_O_RD | GUEST_O_WR | GUEST_O_RDWR |
		       GUEST_O_CREATE)))
		return -1;

	shared_index = find_shared(protocol, name);
	if (shared_index >= 0 && !protocol->cow_done[shared_index]) {
		host_fd = open(protocol->shared_paths[shared_index], O_RDONLY);
	} else {
		if (local_path(protocol, name, path, sizeof(path)) < 0)
			return -1;
		if (!(flags & GUEST_O_CREATE) && access(path, F_OK) < 0)
			return -1;
		host_fd = open(path, access_mode |
			       ((flags & GUEST_O_CREATE) ? O_CREAT : 0), 0644);
		shared_index = -1;
	}
	if (host_fd < 0)
		return -1;

	protocol->files[descriptor] = (struct open_file){
		.used = 1,
		.host_fd = host_fd,
		.flags = flags,
		.shared_index = shared_index
	};
	return descriptor;
}

static int close_guest_file(struct file_protocol *protocol, int descriptor)
{
	if (descriptor < 0 || descriptor >= MAX_OPEN_FILES ||
	    !protocol->files[descriptor].used)
		return -1;
	if (close(protocol->files[descriptor].host_fd) < 0)
		return -1;
	protocol->files[descriptor] = (struct open_file){ 0 };
	return 0;
}

static int copy_file(const char *source, const char *destination)
{
	unsigned char buffer[4096];
	int source_fd = open(source, O_RDONLY);
	int destination_fd;
	ssize_t count;

	if (source_fd < 0)
		return -1;
	destination_fd = open(destination, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (destination_fd < 0) {
		close(source_fd);
		return -1;
	}

	while ((count = read(source_fd, buffer, sizeof(buffer))) > 0) {
		ssize_t written = 0;
		while (written < count) {
			ssize_t result = write(destination_fd, buffer + written,
					       (size_t)(count - written));
			if (result <= 0) {
				close(destination_fd);
				close(source_fd);
				return -1;
			}
			written += result;
		}
	}

	int destination_close = close(destination_fd);
	int source_close = close(source_fd);
	if (destination_close < 0 || source_close < 0 || count < 0)
		return -1;
	return 0;
}

static int switch_to_private_copy(struct file_protocol *protocol,
				  int shared_index)
{
	char path[PATH_MAX];
	const char *name = file_name(protocol->shared_paths[shared_index]);
	int replacements[MAX_OPEN_FILES];

	if (protocol->cow_done[shared_index])
		return 0;
	if (local_path(protocol, name, path, sizeof(path)) < 0 ||
	    copy_file(protocol->shared_paths[shared_index], path) < 0)
		return -1;

	for (int i = 0; i < MAX_OPEN_FILES; ++i)
		replacements[i] = -1;

	for (int i = 0; i < MAX_OPEN_FILES; ++i) {
		struct open_file *file = &protocol->files[i];
		if (!file->used || file->shared_index != shared_index)
			continue;

		off_t offset = lseek(file->host_fd, 0, SEEK_CUR);
		int new_fd = open(path, guest_access_mode(file->flags));
		if (offset < 0 || new_fd < 0 ||
		    lseek(new_fd, offset, SEEK_SET) < 0) {
			if (new_fd >= 0)
				close(new_fd);
			for (int j = 0; j < MAX_OPEN_FILES; ++j) {
				if (replacements[j] >= 0)
					close(replacements[j]);
			}
			return -1;
		}
		replacements[i] = new_fd;
	}

	for (int i = 0; i < MAX_OPEN_FILES; ++i) {
		struct open_file *file = &protocol->files[i];
		if (replacements[i] < 0)
			continue;
		close(file->host_fd);
		file->host_fd = replacements[i];
		file->shared_index = -1;
	}

	protocol->cow_done[shared_index] = 1;
	return 0;
}

static int read_guest_file(struct file_protocol *protocol, int descriptor,
			   unsigned char *buffer, uint32_t count)
{
	struct open_file *file;
	ssize_t result;

	if (descriptor < 0 || descriptor >= MAX_OPEN_FILES ||
	    !protocol->files[descriptor].used)
		return -1;
	file = &protocol->files[descriptor];
	if (!(file->flags & (GUEST_O_RD | GUEST_O_RDWR)))
		return -1;
	result = read(file->host_fd, buffer, count);
	return result >= 0 && result <= INT_MAX ? (int)result : -1;
}

static int write_guest_file(struct file_protocol *protocol, int descriptor,
			    const unsigned char *buffer, uint32_t count)
{
	struct open_file *file;
	ssize_t result;

	if (descriptor < 0 || descriptor >= MAX_OPEN_FILES ||
	    !protocol->files[descriptor].used)
		return -1;
	file = &protocol->files[descriptor];
	if (!(file->flags & (GUEST_O_WR | GUEST_O_RDWR)))
		return -1;
	if (file->shared_index >= 0 &&
	    switch_to_private_copy(protocol, file->shared_index) < 0)
		return -1;

	file = &protocol->files[descriptor];
	result = write(file->host_fd, buffer, count);
	return result >= 0 && result <= INT_MAX ? (int)result : -1;
}

static int seek_guest_file(struct file_protocol *protocol, int descriptor,
			   int32_t offset, int off_flag)
{
	int whence;
	off_t result;

	if (descriptor < 0 || descriptor >= MAX_OPEN_FILES ||
	    !protocol->files[descriptor].used)
		return -1;
	if (off_flag == GUEST_SEEK_SET)
		whence = SEEK_SET;
	else if (off_flag == GUEST_SEEK_END) {
		whence = SEEK_END;
		offset = 0;
	} else
		return -1;

	result = lseek(protocol->files[descriptor].host_fd, offset, whence);
	return result >= 0 && result <= INT_MAX ? (int)result : -1;
}

static int queue_response(struct file_protocol *protocol, int result,
			  const unsigned char *data, size_t data_size)
{
	int32_t result32 = result;

	protocol->response = malloc(sizeof(result32) + data_size);
	if (!protocol->response)
		return -1;
	memcpy(protocol->response, &result32, sizeof(result32));
	if (data_size)
		memcpy(protocol->response + sizeof(result32), data, data_size);
	protocol->response_length = sizeof(result32) + data_size;
	protocol->response_position = 0;
	return 0;
}

static void reset_request(struct file_protocol *protocol)
{
	free(protocol->payload);
	protocol->payload = NULL;
	protocol->operation = 0;
	protocol->expected_args = 0;
	protocol->arg_count = 0;
	protocol->expected_payload = 0;
	protocol->payload_length = 0;
}

static int execute_request(struct file_protocol *protocol)
{
	unsigned char *read_buffer = NULL;
	int result = -1;
	size_t response_data_size = 0;

	switch (protocol->operation) {
	case FILE_OPEN: {
		char name[MAX_NAME_LENGTH + 1];
		memcpy(name, protocol->payload, protocol->payload_length);
		name[protocol->payload_length] = '\0';
		result = open_guest_file(protocol, name, (int)protocol->args[0]);
		break;
	}
	case FILE_CLOSE:
		result = close_guest_file(protocol, (int32_t)protocol->args[0]);
		break;
	case FILE_READ:
		if (protocol->args[1] > MAX_TRANSFER_SIZE)
			break;
		if (protocol->args[1]) {
			read_buffer = malloc(protocol->args[1]);
			if (!read_buffer)
				goto done;
		}
		result = read_guest_file(protocol, (int32_t)protocol->args[0],
					 read_buffer, protocol->args[1]);
		if (result > 0)
			response_data_size = (size_t)result;
		break;
	case FILE_WRITE:
		result = write_guest_file(protocol, (int32_t)protocol->args[0],
					  protocol->payload, protocol->args[1]);
		break;
	case FILE_LSEEK:
		result = seek_guest_file(protocol, (int32_t)protocol->args[0],
					 (int32_t)protocol->args[1],
					 (int)protocol->args[2]);
		break;
	}

done:
	if (queue_response(protocol, result, read_buffer, response_data_size) < 0) {
		free(read_buffer);
		return -1;
	}
	free(read_buffer);
	reset_request(protocol);
	return 0;
}

static int start_request(struct file_protocol *protocol, uint32_t operation)
{
	protocol->operation = operation;
	switch (operation) {
	case FILE_OPEN:
	case FILE_READ:
	case FILE_WRITE:
		protocol->expected_args = 2;
		return 0;
	case FILE_CLOSE:
		protocol->expected_args = 1;
		return 0;
	case FILE_LSEEK:
		protocol->expected_args = 3;
		return 0;
	default:
		return -1;
	}
}

static int receive_output(struct file_protocol *protocol,
			  const unsigned char *data, unsigned size)
{
	if (protocol->response)
		return -1;

	if (!protocol->operation) {
		uint32_t operation;
		if (size != sizeof(operation))
			return -1;
		memcpy(&operation, data, sizeof(operation));
		return start_request(protocol, operation);
	}

	if (protocol->arg_count < protocol->expected_args) {
		uint32_t value;
		if (size != sizeof(value))
			return -1;
		memcpy(&value, data, sizeof(value));
		protocol->args[protocol->arg_count++] = value;
		if (protocol->arg_count < protocol->expected_args)
			return 0;

		if (protocol->operation == FILE_OPEN) {
			if (!protocol->args[1] ||
			    protocol->args[1] > MAX_NAME_LENGTH)
				return -1;
			protocol->expected_payload = protocol->args[1];
		} else if (protocol->operation == FILE_WRITE) {
			if (protocol->args[1] > MAX_TRANSFER_SIZE)
				return -1;
			protocol->expected_payload = protocol->args[1];
		}

		if (protocol->expected_payload) {
			protocol->payload = malloc(protocol->expected_payload);
			return protocol->payload ? 0 : -1;
		}
		return execute_request(protocol);
	}

	if (size != 1 || protocol->payload_length >= protocol->expected_payload)
		return -1;
	protocol->payload[protocol->payload_length++] = *data;
	if (protocol->payload_length == protocol->expected_payload)
		return execute_request(protocol);
	return 0;
}

static int send_input(struct file_protocol *protocol, unsigned char *data,
		      unsigned size)
{
	if (!protocol->response ||
	    protocol->response_position + size > protocol->response_length)
		return -1;
	if ((protocol->response_position == 0 && size != sizeof(int32_t)) ||
	    (protocol->response_position != 0 && size != 1))
		return -1;

	memcpy(data, protocol->response + protocol->response_position, size);
	protocol->response_position += size;
	if (protocol->response_position == protocol->response_length) {
		free(protocol->response);
		protocol->response = NULL;
		protocol->response_length = 0;
		protocol->response_position = 0;
	}
	return 0;
}

struct file_protocol *file_protocol_create(const char *local_dir,
					   const char **shared_paths,
					   size_t shared_count)
{
	struct file_protocol *protocol = calloc(1, sizeof(*protocol));

	if (!protocol ||
	    snprintf(protocol->local_dir, sizeof(protocol->local_dir), "%s",
		     local_dir) >= (int)sizeof(protocol->local_dir)) {
		free(protocol);
		return NULL;
	}
	protocol->shared_paths = shared_paths;
	protocol->shared_count = shared_count;
	if (shared_count) {
		protocol->cow_done = calloc(shared_count,
					    sizeof(*protocol->cow_done));
		if (!protocol->cow_done) {
			free(protocol);
			return NULL;
		}
	}
	return protocol;
}

void file_protocol_destroy(struct file_protocol *protocol)
{
	if (!protocol)
		return;
	for (int i = 0; i < MAX_OPEN_FILES; ++i) {
		if (protocol->files[i].used)
			close(protocol->files[i].host_fd);
	}
	free(protocol->response);
	free(protocol->payload);
	free(protocol->cow_done);
	free(protocol);
}

int file_protocol_handle_io(struct file_protocol *protocol,
			    struct kvm_run *run)
{
	unsigned char *data = (unsigned char *)run + run->io.data_offset;

	if (run->io.port != FILE_IO_PORT || run->io.count != 1)
		return -1;
	if (run->io.direction == KVM_EXIT_IO_OUT)
		return receive_output(protocol, data, run->io.size);
	if (run->io.direction == KVM_EXIT_IO_IN)
		return send_input(protocol, data, run->io.size);
	return -1;
}
