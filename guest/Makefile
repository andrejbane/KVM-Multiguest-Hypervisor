CC = gcc

BUILD_DIR ?= build

SRCS = $(wildcard src/*.c)
OBJS = $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SRCS))
DEPS = $(OBJS:.o=.d)

IMG = $(BUILD_DIR)/guest.img

.PHONY: all clean

all: $(BUILD_DIR) $(IMG)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) -m64 -ffreestanding -fno-pic -fno-stack-protector -fno-asynchronous-unwind-tables -fno-builtin -mno-red-zone -mgeneral-regs-only -Iinc -MMD -MP -c -o $@ $<

$(IMG): $(OBJS) guest.ld
	ld -T guest.ld $(OBJS) -o $@

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
