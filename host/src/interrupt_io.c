#include "interrupt_io.h"

#include <linux/kvm.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct interrupt_shared {
	pthread_mutex_t lock;
	pthread_cond_t changed;
	unsigned char buffer[BUFFER_SIZE];
	uint32_t size;
	size_t reader_count;
	size_t readers_done;
	unsigned long generation;
	int writer_ready;
	int aborted;
};

struct interrupt_vm {
	struct interrupt_shared *shared;
	int mode;
	int role_sent;
	int done;
	unsigned long generation;

	uint32_t writer_requested;
	uint32_t writer_received;
	uint32_t writer_stored;
	int writer_response_ready;

	uint32_t reader_position;
	int reader_size_sent;
};

static int publish_buffer(struct interrupt_vm *vm)
{
	struct interrupt_shared *shared = vm->shared;

	if (pthread_mutex_lock(&shared->lock))
		return -1;
	shared->size = vm->writer_stored;
	shared->readers_done = 0;
	++shared->generation;
	shared->writer_ready = shared->reader_count == 0;
	pthread_cond_broadcast(&shared->changed);
	pthread_mutex_unlock(&shared->lock);
	vm->writer_response_ready = 1;
	return 0;
}

struct interrupt_shared *interrupt_shared_create(size_t vm_count)
{
	struct interrupt_shared *shared = calloc(1, sizeof(*shared));

	if (!shared)
		return NULL;
	if (pthread_mutex_init(&shared->lock, NULL)) {
		free(shared);
		return NULL;
	}
	if (pthread_cond_init(&shared->changed, NULL)) {
		pthread_mutex_destroy(&shared->lock);
		free(shared);
		return NULL;
	}
	shared->reader_count = vm_count > 0 ? vm_count - 1 : 0;
	shared->writer_ready = 1;
	return shared;
}

void interrupt_shared_destroy(struct interrupt_shared *shared)
{
	if (!shared)
		return;
	pthread_cond_destroy(&shared->changed);
	pthread_mutex_destroy(&shared->lock);
	free(shared);
}

void interrupt_shared_abort(struct interrupt_shared *shared)
{
	pthread_mutex_lock(&shared->lock);
	shared->aborted = 1;
	pthread_cond_broadcast(&shared->changed);
	pthread_mutex_unlock(&shared->lock);
}

struct interrupt_vm *interrupt_vm_create(struct interrupt_shared *shared,
					 int mode)
{
	struct interrupt_vm *vm = calloc(1, sizeof(*vm));

	if (!vm)
		return NULL;
	vm->shared = shared;
	vm->mode = mode;
	return vm;
}

void interrupt_vm_destroy(struct interrupt_vm *vm)
{
	free(vm);
}

int interrupt_vm_wait_ready(struct interrupt_vm *vm)
{
	struct interrupt_shared *shared = vm->shared;
	int ready;

	if (!vm->role_sent)
		return 0;

	if (pthread_mutex_lock(&shared->lock))
		return -1;
	for (;;) {
		if (shared->aborted) {
			pthread_mutex_unlock(&shared->lock);
			return -1;
		}
		ready = vm->mode == VM_MODE_WRITER ?
			shared->writer_ready :
			shared->generation > vm->generation;
		if (ready)
			break;
		pthread_cond_wait(&shared->changed, &shared->lock);
	}
	pthread_mutex_unlock(&shared->lock);
	return 0;
}

int interrupt_vm_done(const struct interrupt_vm *vm)
{
	return vm->done;
}

int interrupt_vm_started(const struct interrupt_vm *vm)
{
	return vm->role_sent;
}

static int send_role(struct interrupt_vm *vm, unsigned char *data,
		     unsigned size)
{
	uint32_t mode = (uint32_t)vm->mode;

	if (size != sizeof(mode))
		return -1;
	memcpy(data, &mode, sizeof(mode));
	vm->role_sent = 1;
	return 0;
}

static int handle_writer(struct interrupt_vm *vm, struct kvm_run *run,
			 unsigned char *data)
{
	struct interrupt_shared *shared = vm->shared;

	if (run->io.port == BUFFER_PORT &&
	    run->io.direction == KVM_EXIT_IO_OUT) {
		if (vm->writer_response_ready)
			return -1;
		if (!vm->writer_requested && !vm->writer_received) {
			if (run->io.size != sizeof(uint32_t))
				return -1;
			memcpy(&vm->writer_requested, data, sizeof(uint32_t));
			vm->writer_stored = vm->writer_requested < BUFFER_SIZE ?
				vm->writer_requested : BUFFER_SIZE;
			if (!vm->writer_requested)
				return publish_buffer(vm);
			return 0;
		}

		if (run->io.size != 1 ||
		    vm->writer_received >= vm->writer_requested)
			return -1;
		if (vm->writer_received < BUFFER_SIZE)
			shared->buffer[vm->writer_received] = *data;
		++vm->writer_received;
		if (vm->writer_received == vm->writer_requested)
			return publish_buffer(vm);
		return 0;
	}

	if (run->io.port == STATUS_PORT &&
	    run->io.direction == KVM_EXIT_IO_IN &&
	    run->io.size == sizeof(uint32_t) &&
	    vm->writer_response_ready) {
		memcpy(data, &vm->writer_stored, sizeof(uint32_t));
		if (!vm->writer_stored)
			vm->done = 1;
		vm->writer_requested = 0;
		vm->writer_received = 0;
		vm->writer_stored = 0;
		vm->writer_response_ready = 0;
		return 0;
	}
	return -1;
}

static int handle_reader(struct interrupt_vm *vm, struct kvm_run *run,
			 unsigned char *data)
{
	struct interrupt_shared *shared = vm->shared;

	if (run->io.port == BUFFER_PORT &&
	    run->io.direction == KVM_EXIT_IO_IN) {
		if (!vm->reader_size_sent) {
			if (run->io.size != sizeof(uint32_t))
				return -1;
			memcpy(data, &shared->size, sizeof(uint32_t));
			vm->reader_size_sent = 1;
			return 0;
		}
		if (run->io.size != 1 ||
		    vm->reader_position >= shared->size)
			return -1;
		*data = shared->buffer[vm->reader_position++];
		return 0;
	}

	if (run->io.port == STATUS_PORT &&
	    run->io.direction == KVM_EXIT_IO_OUT &&
	    run->io.size == sizeof(uint32_t)) {
		uint32_t read_count;
		memcpy(&read_count, data, sizeof(uint32_t));
		if (read_count != shared->size ||
		    vm->reader_position != shared->size)
			return -1;

		pthread_mutex_lock(&shared->lock);
		if (vm->generation == shared->generation) {
			pthread_mutex_unlock(&shared->lock);
			return -1;
		}
		vm->generation = shared->generation;
		++shared->readers_done;
		if (shared->readers_done >= shared->reader_count) {
			shared->writer_ready = 1;
			pthread_cond_broadcast(&shared->changed);
		}
		pthread_mutex_unlock(&shared->lock);

		if (!read_count)
			vm->done = 1;
		vm->reader_position = 0;
		vm->reader_size_sent = 0;
		return 0;
	}
	return -1;
}

int interrupt_vm_handle_io(struct interrupt_vm *vm, struct kvm_run *run)
{
	unsigned char *data = (unsigned char *)run + run->io.data_offset;

	if (run->io.count != 1)
		return -1;
	if (!vm->role_sent) {
		if (run->io.port != BUFFER_PORT ||
		    run->io.direction != KVM_EXIT_IO_IN)
			return -1;
		return send_role(vm, data, run->io.size);
	}
	return vm->mode == VM_MODE_WRITER ?
		handle_writer(vm, run, data) : handle_reader(vm, run, data);
}
