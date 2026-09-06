#include "file_io.h"
#include "interrupt_io.h"
#include "vm.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/kvm.h>

struct guest_args {
	const char *image_path;
	size_t mem_size;
	size_t page_size;
	const char **shared_paths;
	size_t shared_count;
	char local_dir[PATH_MAX];
	struct interrupt_shared *interrupts;
	int mode;
};

struct serial_state {
	int output_locked;
};

static pthread_mutex_t serial_lock = PTHREAD_MUTEX_INITIALIZER;

static void print_usage(const char *program)
{
	fprintf(stderr,
		"Usage: %s --memory {2|4|8} --page {4|2} "
		"--guest <image> [image ...] [--file <path> [path ...]]\n",
		program);
}

static void finish_serial_output(struct serial_state *state)
{
	if (state->output_locked) {
		fflush(stdout);
		pthread_mutex_unlock(&serial_lock);
		state->output_locked = 0;
	}
}

static int handle_serial_io(struct vm *v, const char *image_path,
			    struct serial_state *state)
{
	uint8_t *data = (uint8_t *)v->run + v->run->io.data_offset;
	int ch;

	if (v->run->io.port != 0xE9 || v->run->io.size != 1 ||
	    v->run->io.count != 1) {
		fprintf(stderr,
			"VM '%s' stopped on unsupported I/O: port=0x%x size=%u count=%u\n",
			image_path, v->run->io.port, v->run->io.size,
			v->run->io.count);
		return -1;
	}

	if (v->run->io.direction == KVM_EXIT_IO_OUT) {
		if (!state->output_locked) {
			pthread_mutex_lock(&serial_lock);
			state->output_locked = 1;
		}
		putchar(*data);
		if (*data == '\n')
			finish_serial_output(state);
		return 0;
	}

	if (v->run->io.direction != KVM_EXIT_IO_IN) {
		fprintf(stderr, "VM '%s' stopped on invalid I/O direction %u\n",
			image_path, v->run->io.direction);
		return -1;
	}

	if (!state->output_locked) {
		pthread_mutex_lock(&serial_lock);
		state->output_locked = 1;
	}
	fflush(stdout);
	ch = getchar();
	if (ch == EOF) {
		fprintf(stderr, "VM '%s' could not read a byte from stdin\n",
			image_path);
		finish_serial_output(state);
		return -1;
	}
	*data = (uint8_t)ch;
	finish_serial_output(state);
	return 0;
}

static void *run_guest(void *opaque)
{
	struct guest_args *args = opaque;
	struct vm v;
	struct kvm_sregs sregs;
	struct kvm_regs regs;
	struct serial_state serial = { 0 };
	struct file_protocol *files;
	struct interrupt_vm *interrupts;

	interrupts = interrupt_vm_create(args->interrupts, args->mode);
	if (!interrupts) {
		fprintf(stderr, "Failed to initialize interrupts for VM '%s'\n",
			args->image_path);
		interrupt_shared_abort(args->interrupts);
		return (void *)(intptr_t)1;
	}

	if (vm_init(&v, args->mem_size, args->page_size)) {
		fprintf(stderr, "Failed to initialize VM '%s'\n", args->image_path);
		interrupt_shared_abort(args->interrupts);
		interrupt_vm_destroy(interrupts);
		vm_destroy(&v);
		return (void *)(intptr_t)1;
	}

	if (ioctl(v.vcpu_fd, KVM_GET_SREGS, &sregs) < 0) {
		perror("KVM_GET_SREGS");
		interrupt_shared_abort(args->interrupts);
		interrupt_vm_destroy(interrupts);
		vm_destroy(&v);
		return (void *)(intptr_t)1;
	}

	setup_long_mode(&v, &sregs);

	if (ioctl(v.vcpu_fd, KVM_SET_SREGS, &sregs) < 0) {
		perror("KVM_SET_SREGS");
		interrupt_shared_abort(args->interrupts);
		interrupt_vm_destroy(interrupts);
		vm_destroy(&v);
		return (void *)(intptr_t)1;
	}

	if (load_guest_image(&v, args->image_path, GUEST_START_ADDR) < 0) {
		fprintf(stderr, "Failed to load guest image '%s'\n",
			args->image_path);
		interrupt_shared_abort(args->interrupts);
		interrupt_vm_destroy(interrupts);
		vm_destroy(&v);
		return (void *)(intptr_t)1;
	}

	memset(&regs, 0, sizeof(regs));
	regs.rflags = 0x2;
	regs.rip    = GUEST_START_ADDR;
	regs.rsp    = v.usable_mem_size;

	if (ioctl(v.vcpu_fd, KVM_SET_REGS, &regs) < 0) {
		perror("KVM_SET_REGS");
		interrupt_shared_abort(args->interrupts);
		interrupt_vm_destroy(interrupts);
		vm_destroy(&v);
		return (void *)(intptr_t)1;
	}

	files = file_protocol_create(args->local_dir, args->shared_paths,
				     args->shared_count);
	if (!files) {
		fprintf(stderr, "Failed to initialize files for VM '%s'\n",
			args->image_path);
		interrupt_shared_abort(args->interrupts);
		interrupt_vm_destroy(interrupts);
		vm_destroy(&v);
		return (void *)(intptr_t)1;
	}

	for (;;) {
		if (ioctl(v.vcpu_fd, KVM_RUN, 0) < 0) {
			perror("KVM_RUN");
			finish_serial_output(&serial);
			interrupt_shared_abort(args->interrupts);
			interrupt_vm_destroy(interrupts);
			file_protocol_destroy(files);
			vm_destroy(&v);
			return (void *)(intptr_t)1;
		}

		switch (v.run->exit_reason) {
		case KVM_EXIT_IO:
			if (v.run->io.port == 0xE9 &&
			    handle_serial_io(&v, args->image_path, &serial) == 0)
				continue;
			if (v.run->io.port == FILE_IO_PORT &&
			    file_protocol_handle_io(files, v.run) == 0)
				continue;
			if ((v.run->io.port == BUFFER_PORT ||
			     v.run->io.port == STATUS_PORT) &&
			    interrupt_vm_handle_io(interrupts, v.run) == 0)
				continue;

			fprintf(stderr,
				"VM '%s' stopped on invalid I/O at port 0x%x\n",
				args->image_path, v.run->io.port);
			{
				finish_serial_output(&serial);
				interrupt_shared_abort(args->interrupts);
				interrupt_vm_destroy(interrupts);
				file_protocol_destroy(files);
				vm_destroy(&v);
				return (void *)(intptr_t)1;
			}
		case KVM_EXIT_HLT:
			finish_serial_output(&serial);
			if (interrupt_vm_done(interrupts)) {
				interrupt_vm_destroy(interrupts);
				file_protocol_destroy(files);
				vm_destroy(&v);
				return NULL;
			}
			if (!v.run->if_flag) {
				if (interrupt_vm_started(interrupts)) {
					fprintf(stderr,
						"VM '%s' halted before completing interrupt transfer\n",
						args->image_path);
					interrupt_shared_abort(args->interrupts);
					interrupt_vm_destroy(interrupts);
					file_protocol_destroy(files);
					vm_destroy(&v);
					return (void *)(intptr_t)1;
				}
				interrupt_shared_abort(args->interrupts);
				interrupt_vm_destroy(interrupts);
				file_protocol_destroy(files);
				vm_destroy(&v);
				return NULL;
			}
			if (interrupt_vm_wait_ready(interrupts) < 0) {
				fprintf(stderr,
					"VM '%s' stopped because the interrupt transfer was aborted\n",
					args->image_path);
				interrupt_vm_destroy(interrupts);
				file_protocol_destroy(files);
				vm_destroy(&v);
				return (void *)(intptr_t)1;
			}
			{
				struct kvm_interrupt irq = {
					.irq = INTERRUPT_VECTOR
				};
				if (ioctl(v.vcpu_fd, KVM_INTERRUPT, &irq) < 0) {
					perror("KVM_INTERRUPT");
					interrupt_shared_abort(args->interrupts);
					interrupt_vm_destroy(interrupts);
					file_protocol_destroy(files);
					vm_destroy(&v);
					return (void *)(intptr_t)1;
				}
			}
			continue;
		case KVM_EXIT_FAIL_ENTRY:
			fprintf(stderr,
				"VM '%s' failed to enter KVM: reason=0x%llx\n",
				args->image_path,
				(unsigned long long)v.run->fail_entry.hardware_entry_failure_reason);
			break;
		case KVM_EXIT_INTERNAL_ERROR:
			fprintf(stderr, "VM '%s' stopped on KVM internal error: %u\n",
				args->image_path, v.run->internal.suberror);
			break;
		case KVM_EXIT_SHUTDOWN:
			fprintf(stderr, "VM '%s' shut down unexpectedly\n",
				args->image_path);
			break;
		default:
			fprintf(stderr, "VM '%s' stopped on unexpected exit code %u\n",
				args->image_path, v.run->exit_reason);
			break;
		}
		finish_serial_output(&serial);
		interrupt_shared_abort(args->interrupts);
		interrupt_vm_destroy(interrupts);
		file_protocol_destroy(files);
		vm_destroy(&v);
		return (void *)(intptr_t)1;
	}
}

static int is_option(const char *argument)
{
	return argument[0] == '-';
}

static const char *base_name(const char *path)
{
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

int main(int argc, char *argv[])
{
	const char **guest_paths = calloc((size_t)argc, sizeof(*guest_paths));
	const char **shared_paths = calloc((size_t)argc, sizeof(*shared_paths));
	struct guest_args *guest_args;
	pthread_t *threads;
	struct interrupt_shared *interrupts;
	size_t mem_size = 0;
	size_t page_size = 0;
	size_t guest_count = 0;
	size_t shared_count = 0;
	int status = 0;
	char run_dir[PATH_MAX];

	if (!guest_paths || !shared_paths) {
		perror("calloc");
		free(shared_paths);
		free(guest_paths);
		return 1;
	}

	for (int i = 1; i < argc;) {
		if (!strcmp(argv[i], "-m") || !strcmp(argv[i], "--memory")) {
			if (++i >= argc) {
				fprintf(stderr, "Memory value is missing\n");
				status = 1;
				break;
			}
			if (!strcmp(argv[i], "2") || !strcmp(argv[i], "4") ||
			    !strcmp(argv[i], "8"))
				mem_size = (size_t)strtoul(argv[i], NULL, 10) *
					1024u * 1024u;
			else {
				fprintf(stderr, "Memory must be 2, 4, or 8 MB\n");
				status = 1;
			}
			++i;
		} else if (!strcmp(argv[i], "-p") ||
			   !strcmp(argv[i], "--page")) {
			if (++i >= argc) {
				fprintf(stderr, "Page-size value is missing\n");
				status = 1;
				break;
			}
			if (!strcmp(argv[i], "4"))
				page_size = PAGE_SIZE_4K;
			else if (!strcmp(argv[i], "2"))
				page_size = PAGE_SIZE_2M;
			else {
				fprintf(stderr, "Page size must be 4 KB or 2 MB\n");
				status = 1;
			}
			++i;
		} else if (!strcmp(argv[i], "-g") ||
			   !strcmp(argv[i], "--guest")) {
			size_t old_count = guest_count;
			for (++i; i < argc && !is_option(argv[i]); ++i)
				guest_paths[guest_count++] = argv[i];
			if (guest_count == old_count) {
				fprintf(stderr, "At least one path must follow --guest\n");
				status = 1;
			}
		} else if (!strcmp(argv[i], "-f") ||
			   !strcmp(argv[i], "--file")) {
			size_t old_count = shared_count;
			for (++i; i < argc && !is_option(argv[i]); ++i)
				shared_paths[shared_count++] = argv[i];
			if (shared_count == old_count) {
				fprintf(stderr, "At least one path must follow --file\n");
				status = 1;
			}
		} else {
			fprintf(stderr, "Unknown option or argument: %s\n", argv[i]);
			status = 1;
			++i;
		}
	}

	if (status || !mem_size || !page_size || !guest_count) {
		if (!status)
			fprintf(stderr, "Memory, page size, and at least one guest are required\n");
		print_usage(argv[0]);
		free(shared_paths);
		free(guest_paths);
		return 1;
	}

	for (size_t i = 0; i < shared_count; ++i) {
		struct stat info;
		if (stat(shared_paths[i], &info) < 0 || !S_ISREG(info.st_mode) ||
		    access(shared_paths[i], R_OK) < 0) {
			fprintf(stderr, "Shared file is not readable: %s\n",
				shared_paths[i]);
			status = 1;
		}
		for (size_t j = 0; j < i; ++j) {
			if (!strcmp(base_name(shared_paths[i]),
				    base_name(shared_paths[j]))) {
				fprintf(stderr,
					"Shared filenames must be unique: %s\n",
					base_name(shared_paths[i]));
				status = 1;
			}
		}
	}
	if (status) {
		free(shared_paths);
		free(guest_paths);
		return 1;
	}

	if ((mkdir("vm_files", 0755) < 0 && errno != EEXIST) ||
	    snprintf(run_dir, sizeof(run_dir), "vm_files/run_%ld",
		     (long)getpid()) >= (int)sizeof(run_dir) ||
	    mkdir(run_dir, 0755) < 0) {
		perror("Failed to create VM file directory");
		free(shared_paths);
		free(guest_paths);
		return 1;
	}

	guest_args = calloc(guest_count, sizeof(*guest_args));
	threads = calloc(guest_count, sizeof(*threads));
	interrupts = interrupt_shared_create(guest_count);
	if (!guest_args || !threads || !interrupts) {
		perror("calloc");
		interrupt_shared_destroy(interrupts);
		free(threads);
		free(guest_args);
		free(shared_paths);
		free(guest_paths);
		return 1;
	}

	size_t created = 0;
	for (; created < guest_count; ++created) {
		struct guest_args *args = &guest_args[created];
		args->image_path = guest_paths[created];
		args->mem_size = mem_size;
		args->page_size = page_size;
		args->shared_paths = shared_paths;
		args->shared_count = shared_count;
		args->interrupts = interrupts;
		args->mode = created == 0 ? VM_MODE_WRITER : VM_MODE_READER;
		if (snprintf(args->local_dir, sizeof(args->local_dir),
			     "%s/vm_%zu", run_dir, created) >=
			    (int)sizeof(args->local_dir) ||
		    mkdir(args->local_dir, 0755) < 0) {
			perror("Failed to create local VM directory");
			interrupt_shared_abort(interrupts);
			status = 1;
			break;
		}
		int error = pthread_create(&threads[created], NULL, run_guest,
					   args);
		if (error) {
			fprintf(stderr, "pthread_create failed: %s\n", strerror(error));
			interrupt_shared_abort(interrupts);
			status = 1;
			break;
		}
	}

	for (size_t i = 0; i < created; ++i) {
		void *result;
		int error = pthread_join(threads[i], &result);

		if (error) {
			fprintf(stderr, "pthread_join failed: %s\n", strerror(error));
			status = 1;
		} else if (result) {
			status = 1;
		}
	}

	interrupt_shared_destroy(interrupts);
	free(threads);
	free(guest_args);
	free(shared_paths);
	free(guest_paths);
	return status;
}
