#ifndef FILE_IO_H
#define FILE_IO_H

#include <stddef.h>

struct kvm_run;
struct file_protocol;

#define FILE_IO_PORT 0x0278

struct file_protocol *file_protocol_create(const char *local_dir,
					   const char **shared_paths,
					   size_t shared_count);
void file_protocol_destroy(struct file_protocol *protocol);
int file_protocol_handle_io(struct file_protocol *protocol,
			    struct kvm_run *run);

#endif /* FILE_IO_H */
