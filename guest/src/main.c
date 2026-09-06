#include "descriptors.h"
#include "file.h"
#include "interrupts.h"
#include "io.h"

static struct gdt_entry gdt[3];

static void print(const char *text)
{
	while (*text)
		outb(0xE9, *text++);
}

static void print_buffer(const char *buffer, int count)
{
	for (int i = 0; i < count; ++i)
		outb(0xE9, buffer[i]);
	outb(0xE9, '\n');
}

void test_level_1(void)
{
	print("Type one character: ");
	uint8_t character = inb(0xE9);
	print("\nGuest received: ");
	outb(0xE9, character);
	outb(0xE9, '\n');
}

void test_level_2(void)
{
	const char message[] = "Hello from guest";
	char buffer[64];
	int fd = open("local.txt", O_WR | O_CREATE);

	if (fd < 0 || write(fd, message, sizeof(message) - 1) < 0) {
		print("Local file test failed\n");
	} else {
		close(fd);
		fd = open("local.txt", O_RD);
		if (fd < 0 ||
		    lseek(fd, 123, SEEK_END) != (int)sizeof(message) - 1 ||
		    lseek(fd, 0, SEEK_SET) < 0) {
			print("Local file seek test failed\n");
		} else {
			int count = read(fd, buffer, sizeof(buffer));
			print("Local file: ");
			if (count >= 0)
				print_buffer(buffer, count);
			else
				print("read failed\n");
		}
	}
	if (fd >= 0)
		close(fd);
	int invalid_fd = open("1invalid.txt", O_WR | O_CREATE);
	if (invalid_fd >= 0) {
		print("Invalid filename test failed\n");
		close(invalid_fd);
	}

	fd = open("shared.txt", O_RDWR);
	if (fd >= 0) {
		int count = read(fd, buffer, sizeof(buffer));
		print("Shared before COW: ");
		if (count >= 0)
			print_buffer(buffer, count);
		else
			print("read failed\n");

		if (lseek(fd, 0, SEEK_SET) >= 0 &&
		    write(fd, "VM", 2) == 2 &&
		    lseek(fd, 0, SEEK_SET) >= 0) {
			count = read(fd, buffer, sizeof(buffer));
			print("Shared after COW: ");
			if (count >= 0)
				print_buffer(buffer, count);
			else
				print("read failed\n");
		}
		close(fd);
	}
}

void test_level_3(void)
{
	asm volatile("sti");
	while (!interrupts_finished())
		asm volatile("hlt");
	asm volatile("cli");
}

void
__attribute__((noreturn))
__attribute__((section(".start")))
_start(void)
{
	struct dt_ptr p;

	gdt[0] = (struct gdt_entry){ 0 };
	gdt[1] = (struct gdt_entry){  /* 64-bit code, selector 0x08: P=1, DPL=0, S=1, type=0xA, L=1, G=1 */
		.limit_low   = 0xFFFF,
		.access      = 0x9A,
		.flags_limit = 0xAF,
	};
	gdt[2] = (struct gdt_entry){  /* 64-bit data, selector 0x10: P=1, DPL=0, S=1, type=0x2, D/B=1, G=1 */
		.limit_low   = 0xFFFF,
		.access      = 0x92,
		.flags_limit = 0xCF,
	};

	p.limit = sizeof(gdt) - 1;
	p.base  = (uint64_t)(uintptr_t)gdt;
	asm volatile("lgdt %0" : : "m"(p) : "memory");

	/* Reload CS to 0x08 and continue at label 1 */
	asm volatile(
		"pushq $0x08\n\t"
		"lea 1f(%%rip), %%rax\n\t"
		"pushq %%rax\n\t"
		"lretq\n\t"
		"1:\n\t"
		::: "rax", "memory"
	);

	/* Reload data segment selectors to 0x10 */
	asm volatile(
		"movl $0x10, %%eax\n\t"
		"movw %%ax, %%ds\n\t"
		"movw %%ax, %%es\n\t"
		"movw %%ax, %%ss\n\t"
		::: "eax", "memory"
	);
	init_idt();

	test_level_3();

	asm volatile("hlt");
	__builtin_unreachable();
}
