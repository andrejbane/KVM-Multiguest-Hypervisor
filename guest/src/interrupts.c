#include "descriptors.h"
#include "file.h"
#include "interrupts.h"
#include "io.h"

#define BUFFER_PORT 0x510
#define STATUS_PORT 0x520

#define MODE_READER 0
#define MODE_WRITER 1

static struct idt_entry idt[IDT_ENTRIES];
static volatile int mode = -1;
static volatile int finished;
static int file_descriptor = -1;

static void print(const char *s)
{
	while (*s)
		outb(0xE9, *s++);
}

static void handle_writer(void)
{
	char buffer[BUFFER_SIZE];
	int count;
	uint32_t written;

	if (file_descriptor < 0) {
		file_descriptor = open("shared.txt", O_RD);
		if (file_descriptor < 0) {
			print("Writer could not open shared.txt\n");
			outl(BUFFER_PORT, 0);
			(void)inl(STATUS_PORT);
			finished = 1;
			return;
		}
	}
	count = file_descriptor < 0 ? 0 :
		read(file_descriptor, buffer, BUFFER_SIZE);
	if (count < 0)
		count = 0;

	outl(BUFFER_PORT, (uint32_t)count);
	for (int i = 0; i < count; ++i)
		outb(BUFFER_PORT, (uint8_t)buffer[i]);
	written = inl(STATUS_PORT);
	if (written != (uint32_t)count) {
		print("Writer transfer failed\n");
		finished = 1;
	} else if (!count) {
		if (file_descriptor >= 0)
			close(file_descriptor);
		print("Writer finished\n");
		finished = 1;
	}
}

static void handle_reader(void)
{
	char buffer[BUFFER_SIZE];
	uint32_t count = inl(BUFFER_PORT);
	uint32_t read_count = 0;

	while (read_count < count) {
		if (read_count < BUFFER_SIZE)
			buffer[read_count] = (char)inb(BUFFER_PORT);
		else
			(void)inb(BUFFER_PORT);
		++read_count;
	}
	outl(STATUS_PORT, read_count);

	if (read_count != count) {
		print("Reader transfer failed\n");
		finished = 1;
		return;
	}
	if (!count) {
		if (file_descriptor >= 0)
			close(file_descriptor);
		print("Reader finished\n");
		finished = 1;
		return;
	}

	if (file_descriptor < 0)
		file_descriptor = open("received.txt", O_WR | O_CREATE);
	if (file_descriptor < 0 ||
	    write(file_descriptor, buffer,
		  (int)(count < BUFFER_SIZE ? count : BUFFER_SIZE)) !=
	    (int)(count < BUFFER_SIZE ? count : BUFFER_SIZE)) {
		print("Reader file write failed\n");
		finished = 1;
	}
}

/*
	"general-regs-only" sprečava GCC da emituje SSE instrukcije, koje su zabranjene unutar
	__attribute__((interrupt)) handlera
*/
static void __attribute__((interrupt, target("general-regs-only")))
irq0_handler(struct interrupt_frame *frame)
{
	(void)frame;

	if (mode < 0) {
		mode = (int)inl(BUFFER_PORT);
		print(mode == MODE_WRITER ? "Writer VM\n" : "Reader VM\n");
		return;
	}

	if (mode == MODE_WRITER)
		handle_writer();
	else if (mode == MODE_READER)
		handle_reader();
	else
		finished = 1;
}

static void set_idt_gate(unsigned n, void (*handler)(struct interrupt_frame *))
{
	uint64_t addr = (uint64_t)(uintptr_t)handler;
	idt[n].offset_low  = addr & 0xFFFF;
	idt[n].selector    = 0x08;  /* 64-bit code segment */
	idt[n].ist         = 0;
	idt[n].type_attr   = 0x8E;  /* P=1, DPL=0, 64-bit interrupt gate */
	idt[n].offset_mid  = (addr >> 16) & 0xFFFF;
	idt[n].offset_high = (addr >> 32) & 0xFFFFFFFF;
	idt[n].reserved    = 0;
}

void init_idt(void)
{
	struct dt_ptr p;

	set_idt_gate(32, irq0_handler);

	p.limit = sizeof(idt) - 1;
	p.base  = (uint64_t)(uintptr_t)idt;
	asm volatile("lidt %0" : : "m"(p) : "memory");
}

int interrupts_finished(void)
{
	return finished;
}
