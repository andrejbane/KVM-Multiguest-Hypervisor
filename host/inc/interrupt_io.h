#ifndef INTERRUPT_IO_H
#define INTERRUPT_IO_H

#include <stddef.h>

struct interrupt_shared;
struct interrupt_vm;
struct kvm_run;

#define INTERRUPT_VECTOR 32
#define BUFFER_PORT      0x510
#define STATUS_PORT      0x520
#define BUFFER_SIZE      64

#define VM_MODE_READER 0
#define VM_MODE_WRITER 1

struct interrupt_shared *interrupt_shared_create(size_t vm_count);
void interrupt_shared_destroy(struct interrupt_shared *shared);
void interrupt_shared_abort(struct interrupt_shared *shared);

struct interrupt_vm *interrupt_vm_create(struct interrupt_shared *shared,
					 int mode);
void interrupt_vm_destroy(struct interrupt_vm *vm);
int interrupt_vm_handle_io(struct interrupt_vm *vm, struct kvm_run *run);
int interrupt_vm_wait_ready(struct interrupt_vm *vm);
int interrupt_vm_done(const struct interrupt_vm *vm);
int interrupt_vm_started(const struct interrupt_vm *vm);

#endif /* INTERRUPT_IO_H */
