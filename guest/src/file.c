#include "file.h"
#include "io.h"

#include <stdint.h>

#define FILE_PORT 0x0278
#define MAX_NAME_LENGTH 255

enum file_operation {
	FILE_OPEN = 1,
	FILE_CLOSE,
	FILE_READ,
	FILE_WRITE,
	FILE_LSEEK
};

int open(const char *path, int flags)
{
	uint32_t length = 0;

	if (!path)
		return -1;
	while (path[length]) {
		if (length == MAX_NAME_LENGTH)
			return -1;
		++length;
	}
	if (!length)
		return -1;

	outl(FILE_PORT, FILE_OPEN);
	outl(FILE_PORT, (uint32_t)flags);
	outl(FILE_PORT, length);
	for (uint32_t i = 0; i < length; ++i)
		outb(FILE_PORT, (uint8_t)path[i]);
	return (int32_t)inl(FILE_PORT);
}

int close(int fd)
{
	outl(FILE_PORT, FILE_CLOSE);
	outl(FILE_PORT, (uint32_t)fd);
	return (int32_t)inl(FILE_PORT);
}

int read(int fd, char *buf, int count)
{
	int result;

	if (!buf || count < 0)
		return -1;
	outl(FILE_PORT, FILE_READ);
	outl(FILE_PORT, (uint32_t)fd);
	outl(FILE_PORT, (uint32_t)count);
	result = (int32_t)inl(FILE_PORT);
	if (result < 0 || result > count)
		return -1;
	for (int i = 0; i < result; ++i)
		buf[i] = (char)inb(FILE_PORT);
	return result;
}

int write(int fd, const char *buf, int count)
{
	if (!buf || count < 0)
		return -1;
	outl(FILE_PORT, FILE_WRITE);
	outl(FILE_PORT, (uint32_t)fd);
	outl(FILE_PORT, (uint32_t)count);
	for (int i = 0; i < count; ++i)
		outb(FILE_PORT, (uint8_t)buf[i]);
	return (int32_t)inl(FILE_PORT);
}

int lseek(int fd, const int offset, int off_flag)
{
	outl(FILE_PORT, FILE_LSEEK);
	outl(FILE_PORT, (uint32_t)fd);
	outl(FILE_PORT, (uint32_t)offset);
	outl(FILE_PORT, (uint32_t)off_flag);
	return (int32_t)inl(FILE_PORT);
}
