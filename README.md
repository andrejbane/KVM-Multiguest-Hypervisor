# KVM Multi-Guest Hypervisor

This project is a small Linux/KVM hypervisor and freestanding x86-64 guest used
to demonstrate virtual-machine creation, guest/host port I/O, isolated file
access, copy-on-write shared files, and interrupt-driven communication between
multiple virtual machines.

The host starts one thread and one virtual CPU for each guest image. VM 0 acts
as the writer in the inter-VM transfer protocol, while every other VM acts as a
reader.

## Contents

- [Repository layout](#repository-layout)
- [Architecture and execution flow](#architecture-and-execution-flow)
- [Implemented functionality](#implemented-functionality)
- [Requirements](#requirements)
- [Build](#build)
- [Run](#run)
- [Guest test levels](#guest-test-levels)
- [File-service API](#file-service-api)
- [Inter-VM interrupt protocol](#inter-vm-interrupt-protocol)
- [Generated files](#generated-files)
- [Current scope and limits](#current-scope-and-limits)

## Repository layout

| Path | Purpose |
|---|---|
| `host\src\main.c` | Command-line parsing, per-guest threads, KVM run loop, serial I/O, and interrupt injection. |
| `host\src\vm.c` | KVM VM/vCPU creation, guest memory mapping, long-mode page tables, and image loading. |
| `host\src\file_io.c` | Host side of the guest file protocol, descriptor management, isolation, and copy-on-write. |
| `host\src\interrupt_io.c` | Shared writer/reader buffer and synchronization between guest threads. |
| `host\inc\` | Host interfaces and KVM-related constants. |
| `guest\src\main.c` | Freestanding guest entry point and the three demonstration levels. |
| `guest\src\file.c` | Guest-side `open`, `close`, `read`, `write`, and `lseek` wrappers implemented with port I/O. |
| `guest\src\interrupts.c` | IDT setup, interrupt handler, and writer/reader transfer logic. |
| `guest\inc\` | Port-I/O helpers, descriptor layouts, file flags, and interrupt declarations. |
| `guest\guest.ld` | Linker script that creates a flat guest binary beginning with the `.start` section. |
| `host\Makefile` | Builds the userspace hypervisor and links POSIX threads. |
| `guest\Makefile` | Builds the freestanding guest image. |
| `vm_files\` | Runtime-created, per-run and per-VM private file trees. |

## Architecture and execution flow

```text
freestanding guest C sources
            |
            v
     guest/build/guest.img
            |
            | --guest (one or more images)
            v
 host/build/hypervisor
            |
            +-- VM thread 0: writer
            |      |
            |      +-- KVM vCPU + private guest RAM
            |
            +-- VM thread 1..N: readers
                   |
                   +-- KVM vCPU + private guest RAM

Guest port I/O exits to the host:

  0x0E9  serial input/output
  0x0278 file operations
  0x0510 shared transfer buffer and VM role
  0x0520 transfer status
```

The complete lifecycle is:

1. The hypervisor parses the selected memory size, page size, guest images, and
   optional shared files.
2. It creates `vm_files/run_<pid>\vm_<index>` for every VM. Files created by a
   guest are confined to that VM's directory.
3. A shared synchronization object is created, followed by one POSIX thread per
   guest.
4. Each thread opens `/dev/kvm`, creates a VM and one vCPU, maps guest RAM, and
   registers it with `KVM_SET_USER_MEMORY_REGION`.
5. The host constructs x86-64 long-mode page tables using either 4 KiB or 2 MiB
   pages, loads the flat guest image at physical address zero, initializes RIP
   and RSP, and enters the `KVM_RUN` loop.
6. Guest `in` and `out` instructions cause KVM I/O exits. The host dispatches
   them to the serial, file, or inter-VM interrupt protocol.
7. VM 0 reads `shared.txt` in chunks and publishes each chunk. Reader VMs are
   woken with interrupt vector 32, consume the same chunk, and acknowledge it.
   The writer cannot publish the next chunk until all readers finish.
8. A zero-length chunk marks the end of the transfer. The guests halt, their
   threads release KVM and file resources, and the process returns a nonzero
   status if any VM failed.

## Implemented functionality

### Virtual-machine setup

- Linux KVM API version validation.
- One independently allocated VM, vCPU, and memory region per guest image.
- Configurable guest RAM of 2, 4, or 8 MiB.
- Identity-mapped x86-64 long mode using either 4 KiB or 2 MiB pages.
- Flat binary loading at guest physical address `0x00000000`.
- Guest stack initialization below the page-table area.
- Detailed handling of KVM I/O, halt, shutdown, failed-entry, and internal-error
  exits.

### Guest console

Port `0xE9` provides byte-oriented console I/O:

- `outb(0xE9, value)` prints one character.
- `inb(0xE9)` reads one character from standard input.
- A mutex keeps output from concurrent VM threads from interleaving within a
  line.

### Virtual file service

The guest uses port `0x278` to request file operations from the host:

- `open`
- `close`
- `read`
- `write`
- `lseek`

Every VM receives its own file directory. A file supplied with `--file` is
initially shared read-only by basename. On the first guest write, the host
copies it into that VM's directory and transparently moves that VM's open
descriptors to the private copy. Other VMs and the original host file remain
unchanged.

### Interrupt-driven inter-VM transfer

- The first guest is assigned the writer role; all later guests are readers.
- The host coordinates the threads with a mutex, condition variable, and
  generation counter.
- Data is broadcast in chunks of at most 64 bytes.
- Readers receive interrupt vector 32 through `KVM_INTERRUPT`.
- The guest installs a 64-bit IDT entry and performs work in an interrupt
  handler compiled without general SSE register use.
- Every reader acknowledges each generation before the writer continues.
- Failure in one VM aborts the shared protocol and wakes blocked peers.

## Requirements

The host depends on Linux KVM and cannot run as a native Windows executable.
Use a native x86-64 Linux installation or WSL2 with nested virtualization and
`/dev/kvm` support.

Required tools and capabilities:

- x86-64 CPU with hardware virtualization enabled.
- Linux kernel with KVM support.
- Read/write permission for `/dev/kvm`.
- GCC, GNU Make, GNU `ld`, and standard Linux development headers.
- POSIX threads; linked automatically by the host Makefile.

On Debian or Ubuntu:

```bash
sudo apt update
sudo apt install -y build-essential
```

Confirm that KVM is available:

```bash
test -r /dev/kvm -a -w /dev/kvm && echo "KVM is available"
```

If `/dev/kvm` exists but is not accessible, add the current user to its owning
group and start a new login session:

```bash
sudo usermod -aG kvm "$USER"
```

## Build

Run these commands from the repository root in Linux or WSL:

```bash
make -C guest
make -C host
```

The resulting programs are:

```text
guest/build/guest.img
host/build/hypervisor
```

Rebuild from scratch:

```bash
make -C guest clean
make -C host clean
make -C guest
make -C host
```

The guest is compiled as freestanding 64-bit code without libc, position-
independent code, stack protection, a red zone, or asynchronous unwind tables.
The linker script emits a raw binary image. The host is a normal Linux process
linked with `pthread`.

## Run

### Command-line syntax

```text
host/build/hypervisor \
  --memory {2|4|8} \
  --page {4|2} \
  --guest <image> [image ...] \
  [--file <path> [path ...]]
```

Short options are also accepted:

```text
-m  --memory    guest RAM in MiB: 2, 4, or 8
-p  --page      page size in KiB: 4, or in MiB: 2
-g  --guest     one or more flat guest images
-f  --file      one or more readable host files exposed by basename
```

Options can be supplied in any order, but each `--guest` or `--file` consumes
the following paths until the next option.

### Current interrupt-transfer demonstration

The checked-in guest entry point runs `test_level_3()`. Create its input file
and start one writer plus two readers:

```bash
printf 'interrupt transfer data' > shared.txt

./host/build/hypervisor \
  --memory 8 \
  --page 2 \
  --guest \
    guest/build/guest.img \
    guest/build/guest.img \
    guest/build/guest.img \
  --file shared.txt
```

The same image can be supplied more than once because every path starts an
independent VM. VM 0 reads `shared.txt`; VMs 1 and 2 write the received data to
their private `received.txt` files.

Check the newest run:

```bash
latest=$(ls -td vm_files/run_* | head -1)
find "$latest" -type f -print

cmp shared.txt "$latest/vm_1/received.txt"
cmp shared.txt "$latest/vm_2/received.txt"
```

Both `cmp` commands should complete without output and return status zero.

## Guest test levels

`guest\src\main.c` contains three demonstrations. The `_start` function calls
one of them before its final `hlt`; change that call and rebuild the guest to
select a different level.

### Level 1: serial input and output

Set:

```c
test_level_1();
```

Then run:

```bash
make -C guest
printf 'K' | ./host/build/hypervisor \
  -m 2 -p 4 \
  -g guest/build/guest.img
```

The guest asks for one character and prints the received value.

### Level 2: local files and copy-on-write

Set:

```c
test_level_2();
```

Then run:

```bash
make -C guest
printf 'shared-data' > shared.txt

./host/build/hypervisor \
  -m 4 -p 4 \
  -g guest/build/guest.img guest/build/guest.img \
  -f shared.txt
```

Each guest creates a private `local.txt`, reads the original `shared.txt`, then
writes `VM` at its start. The modified file appears only inside that VM's
directory:

```bash
latest=$(ls -td vm_files/run_* | head -1)
cat "$latest/vm_0/local.txt"
cat "$latest/vm_0/shared.txt"
cat "$latest/vm_1/local.txt"
cat "$latest/vm_1/shared.txt"
cat shared.txt
```

The last command still prints the original `shared-data`, demonstrating that
guest writes use private copies.

### Level 3: interrupt transfer

Set:

```c
test_level_3();
```

Use the three-guest command from the [Run](#run) section. Readers store the
writer's data in their private `received.txt` files.

## File-service API

The guest API is declared in `guest\inc\file.h`:

```c
int open(const char *path, int flags);
int close(int fd);
int read(int fd, char *buf, int count);
int write(int fd, const char *buf, int count);
int lseek(int fd, int offset, int off_flag);
```

Available flags:

| Flag | Meaning |
|---|---|
| `O_RD` | Open for reading. |
| `O_WR` | Open for writing. |
| `O_RDWR` | Open for reading and writing. |
| `O_CREATE` | Create a private file if it does not exist. |
| `SEEK_SET` | Seek to an absolute byte offset. |
| `SEEK_END` | Seek to the end of the file. |

This is a small custom guest ABI, not the Linux syscall ABI. Requests and
responses are serialized through KVM port-I/O exits.

## Inter-VM interrupt protocol

The host and guest share two port numbers:

| Port | Purpose |
|---|---|
| `0x510` | VM role, chunk size, and chunk bytes. |
| `0x520` | Writer result and reader acknowledgement. |

On the first interrupt, each guest reads its assigned role. For every later
generation:

1. The writer sends the chunk length and bytes through port `0x510`.
2. The host publishes the chunk and marks a new generation available.
3. Reader threads wake, and the host injects interrupt vector 32.
4. Each reader obtains the length and bytes from port `0x510`.
5. Readers acknowledge the complete chunk through port `0x520`.
6. After all acknowledgements, the writer receives the stored byte count from
   port `0x520` and can publish the next chunk.

The same mechanism sends a final zero-length chunk so every guest can terminate
cleanly.

## Generated files

Every process run creates:

```text
vm_files/
└── run_<host-pid>/
    ├── vm_0/
    │   └── files created or copied by VM 0
    ├── vm_1/
    │   └── files created or copied by VM 1
    └── ...
```

Build products are stored separately:

```text
guest/build/   guest objects, dependency files, and guest.img
host/build/    host objects, dependency files, and hypervisor
```

`make clean` removes only the corresponding build directory. Runtime data under
`vm_files\` is retained for inspection.

## Current scope and limits

- Linux x86-64 with KVM is required; macOS and native Windows are unsupported.
- Each VM has one vCPU and runs the supplied flat, freestanding guest image.
- The guest receives no BIOS, bootloader, operating system, or general device
  emulation.
- Only the serial, file-service, and interrupt-transfer ports described above
  are supported.
- Guest memory is limited by the CLI to 2, 4, or 8 MiB.
- Page size is limited to 4 KiB or 2 MiB.
- Each VM can keep at most 64 guest file descriptors open.
- A single file transfer is limited to 8 MiB.
- Guest filenames must start with an ASCII letter and may then contain only
  ASCII letters, digits, and dots. Directory traversal is rejected.
- Shared files passed in one invocation must have unique basenames.
- Inter-VM data is transferred in 64-byte chunks.
