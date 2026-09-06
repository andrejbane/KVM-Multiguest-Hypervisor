#ifndef VM_H
#define VM_H

#include <stddef.h>
#include <stdint.h>
#include <linux/kvm.h>

#define GUEST_START_ADDR 0

#define PAGE_SIZE_4K (4u * 1024u)
#define PAGE_SIZE_2M (2u * 1024u * 1024u)

#define PDE64_PRESENT (1u << 0)
#define PDE64_RW      (1u << 1)
#define PDE64_USER    (1u << 2)
#define PDE64_PS      (1u << 7)

#define CR0_PE   (1u << 0)
#define CR0_PG   (1u << 31)
#define CR4_PAE  (1u << 5)
#define EFER_LME (1u << 8)
#define EFER_LMA (1u << 10)

struct vm {
	int kvm_fd;
	int vm_fd;
	int vcpu_fd;
	char *mem;
	size_t page_size;
	size_t mem_size;
	size_t usable_mem_size;
	struct kvm_run *run;
	int run_mmap_size;
};

int  vm_init(struct vm *v, size_t mem_size, size_t page_size);
void vm_destroy(struct vm *v);
void setup_long_mode(struct vm *v, struct kvm_sregs *sregs);
int  load_guest_image(struct vm *v, const char *image_path, uint64_t load_addr);

#endif /* VM_H */
