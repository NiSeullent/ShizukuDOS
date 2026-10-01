# Native 64-bit ShizukuDOS applications

These two applications are independently assembled x86-64 programs for the
standalone ShizukuDOS native track. They execute in ring 3 under Kernel64 and
return actual process exit codes. They do not require Windows 98, Windows DLLs,
the Win64 PE loader, a VMX supervisor, or a network connection.

`SD64` is ShizukuDOS's experimental native application format and ABI. It is not
a DOS `.COM`/`.EXE` format or an `INT 21h` extension. These files cannot run in
classic 16-bit DOS, Windows, or another operating system without an SD64 loader.
The ISO's BIOS 16-bit boot track and its UEFI native 64-bit track are separate
execution paths.

| Application | Behavior | Successful exit |
| --- | --- | --- |
| `HELLO64.SD64` | Computes `100000000 * 100000000` with a 64-bit `MUL`, verifies the full result, formats it as hexadecimal, and prints it | `0` |
| `MEM64.SD64` | Reserves and commits 64 KiB at virtual address `0x0000000200000000` (8 GiB), writes and verifies all 8,192 64-bit values, verifies their 64-bit checksum, prints it, and releases the allocation | `0` |

The 8 GiB address is a **virtual address**, backed by the kernel's page allocator.
The demonstration does not require 8 GiB of physical RAM and makes no claim
that physical memory above 4 GiB is managed by the current kernel.

## Build

From the repository root, with Python 3 and NASM installed:

```sh
python3 samples/dos64/build.py
```

The command creates `build/dos64/HELLO64.SD64`, `MEM64.SD64`, `DOS64.IMG`, and
`build-result.json` containing sizes, hashes, source hashes, and exact assembler
commands. It does not download dependencies or build the optional Win64 runtime.
For an alternate output directory, use `--out /path/to/output`.

The `SHZARC01` initrd carries exactly two files:

```text
\SHZ\DOS64\HELLO64.SD64
\SHZ\DOS64\MEM64.SD64
```

The standalone ISO passes `shz.dos64=1`. Kernel64 launches both files directly
from this archive. A standalone kernel with this DOS-only archive also selects
the native track automatically when no `\SHZ\SYS64\ntdll.dll` exists.

## SD64 ABI v1

- **Image:** headerless NASM flat binary, little-endian x86-64 machine code and
  static data. The first byte is the entry instruction. The loader accepts
  images of 1 through 65,536 bytes and rejects `MZ` images. The source uses
  `bits 64`, `default abs`, and `org 0x400000`.
- **Loading:** image and entry at virtual address `0x400000`, a fresh process
  address space, a user stack, and ring 3 execution. Initial arguments are zero.
  The loader currently maps image pages read/write/execute. This ABI is intended
  for trusted local samples; it is not a hardened arbitrary application loader.
- **System calls:** `SYSCALL`, with the call number in `EAX`. Arguments 1–4 are
  `R10`, `RDX`, `R8`, and `R9`; arguments 5 onward are 64-bit stack slots at
  `[RSP+0x28]`, `[RSP+0x30]`, etc. `EAX` returns a signed 32-bit status, zero for
  success. `RCX` and `R11` are clobbered by `SYSCALL`; treat `RAX` as the result.
  These are Shizuku's private numbers and semantics, not a Windows syscall table.
- **Process handle:** `R10=-1` means the current process for the memory and exit
  calls used here.
- **Console:** call `0x30`, `R10=buffer address`, `RDX=byte count`, UTF-8/ASCII;
  the current implementation prints up to 190 bytes per call to the kernel's
  console and prefixes the process name and PID.
- **Exit:** call `0x01`, `R10=-1`, `EDX=exit code`; this must not return.
- **Allocate:** call `0x07`, `R10=-1`, `RDX=&base`, `R8=0`, `R9=&size`, stack
  argument 5=`0x3000` (`MEM_RESERVE | MEM_COMMIT`), argument 6=`4`
  (`PAGE_READWRITE`). `base` and `size` are writable 64-bit quantities.
- **Release:** call `0x08`, `R10=-1`, `RDX=&base`, `R8=&size`, `R9=0x8000`
  (`MEM_RELEASE`); `size` must initially be zero.

`abi.inc` supplies these constants, `PRINT`/`EXIT` macros, and a small 64-bit
hexadecimal formatter. No library, executable wrapper, or compiler runtime is
linked. Other experimental kernel syscalls are outside this sample ABI contract.

## Runtime results and failures

A successful serial log includes:

```text
HELLO64: 100000000 * 100000000 = 0x002386f26fc10000 (10000000000000000); 64-bit arithmetic PASS
DOS64: HELLO64.SD64 exit=00000000 faulted=0 timeout=0 reaped=0
MEM64: virtual base=0x0000000200000000 bytes=65536 checksum=0x46688aacc1fff000; verify and release PASS
DOS64: MEM64.SD64 exit=00000000 faulted=0 timeout=0 reaped=0
DOS64: completed 2 application(s), 0 failure(s)
SHZ-EXIT:0
```

The application lines carry a `[user ... pid ...]` prefix in the actual log.
`reaped=0` means `proc_wait` succeeded. The kernel marks a run as failed on any
nonzero app exit, process fault, timeout, missing/invalid image, or launch failure.
The native runner allows five seconds per application and one second of teardown
grace. It requests termination on timeout and avoids an unbounded process wait
when teardown does not finish. It continues to the second sample after a failure
and exits the guest with code `1` if either application failed.

`HELLO64` returns `1` on computation/console failure. `MEM64` returns `1` for
allocation failure, `2` for address/pattern/checksum failure, `3` for release
failure, and `4` for console failure. An unhandled processor exception instead
produces the kernel's exception status and `faulted=1`.

The timeout bounds user-mode spins and ordinarily interruptible kernel paths.
It cannot recover from a kernel defect that disables interrupts indefinitely.
This release is an experimental native demonstration, not a promise of general
64-bit DOS application compatibility.

## Execution and containment tests

After building the kernels, run:

```sh
python3 samples/dos64/test_runtime.py
```

The three sequential QEMU TCG runs use the original applications, a first
application containing `UD2` (illegal instruction), and a first application
containing an infinite user-mode loop. The checks require the actual exception
or timeout status, successful execution of `MEM64` afterward, the computed
product/checksum, and the correct aggregate guest exit status. Each guest has
no NIC or disk; a 45-second host deadline also terminates a stuck guest. Serial
logs, commands, image hashes, and results are saved under
`build/dos64/validation/`. These tests use the standalone Multiboot entry path;
the ISO's UEFI boot must be validated separately.
