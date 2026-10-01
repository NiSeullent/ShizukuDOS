# kurazy Native API Specification 1.0

`kurazy` is ShizukuDOS's own freestanding API for real 16-bit DOS programs,
protected 32-bit programs, and long-mode 64-bit programs. These are distinct
execution contracts. A shared family name does not turn x86-64 instructions into
16-bit DOS instructions or make an arbitrary Windows executable run.

Status: implemented release ABI 1.0 (`0x00010000`). The authoritative long-mode
C declarations are `include/kurazy.h`; kernel implementation is
`shizukudos/kernel64/kurazy.c`. Corresponding source and GPL-2.0-only license are
included in the distribution. No Windows DLL, CRT, SDK binary, or proprietary
media is part of the native applications.

## Image names and execution contracts

| Track | Filename | Image format | Actual CPU execution | API |
|---|---|---|---|---|
| Real mode | `HELLO.EXE`, `MODE.EXE`, `COUNT.EXE` | DOS MZ16 with relocations and PSP | 16-bit real mode, CR0.PE=0, CR0.PG=0, EFER.LMA=0 | INT 21h subset |
| Protected mode | `HELLO32.EXE`, `MODE32.EXE`, `COUNT32.EXE` | PE32, i386, no imports | 32-bit protected mode | INT 80h subset |
| Long mode | names ending `64.EXE` | PE32+, AMD64, import-free | x86-64 long mode in ring 3 with private paged address space | kurazy SYSCALL service |
| Legacy native sample | `HELLO64.SD64`, `MEM64.SD64` | SD64 raw code | x86-64 long mode in ring 3 | previous SD64 ABI v1 |

The protected and real tracks execute trusted packaged programs in a bounded,
non-preemptive mode bridge. They are neither process-isolated like the long-mode
track nor a complete DOS or DPMI implementation. The bridge returns to the
64-bit kernel, restoring paging, descriptors, interrupt state, and stack.

The long-mode loader checks MZ/PE headers with the shared overflow-checked PE
parser, requires AMD64 PE32+, executable image and a valid executable entry,
maps headers and sections in a fresh process, and applies section permissions.
It rejects imported DLLs, TLS, load configuration, delay imports, CLR images,
low-alignment images, writable executable sections and images over 1 MiB. The
v1 toolchain uses preferred base `0x400000`; relocation/ASLR is not promised by
this native loader. An `.EXE` extension alone does not select long mode.

## Long-mode binary ABI

The compiler's public C ABI is Microsoft x64: RCX/RDX/R8/R9 for the first four
arguments, 32 bytes of caller-provided shadow space, and 16-byte stack alignment
before CALL. The syscall bridge translates to:

- RAX: service number; R10, RDX, R8, R9: up to four arguments.
- RAX on return: signed 32-bit status sign-extended to 64 bits. Read, directory,
  write and tree operations return a nonnegative count on success.
- SYSCALL clobbers RCX and R11 as specified by the CPU. All pointer arguments
  address the caller's user address space, never kernel memory.
- Integers are little-endian. Output structs have natural 8-byte alignment;
  reserved fields are zero. ABI structures contain fixed-width integers and
  byte arrays, never raw kernel pointers.

`kurazy_info` is 72 bytes, `kurazy_dirent` 144 bytes, and
`kurazy_thread_info` 32 bytes. Call `kurazy_query` before using optional features.
`size` and `version` report the implemented ABI; `mode` is LONG64. `cr0` and
`efer` are actual kernel snapshots, not simulated compatibility flags. GOP
width/height are zero when no firmware framebuffer is available.

| Service | Number | Arguments | Result |
|---|---|---|---|
| Query | `0x7000` | info pointer, exact 72-byte size | status; version, capabilities, memory, mode, timing, GOP |
| Write | `0x7001` | bytes pointer, byte count | bytes written to serial console |
| Ticks | `0x7002` | u64 milliseconds pointer | status |
| Allocate | `0x7003` | requested bytes, user pointer to u64 address | page-aligned zero-filled read/write private memory |
| Free | `0x7004` | exact allocation base | status |
| Read | `0x7005` | NUL-terminated path, buffer, capacity, offset | bytes read; zero at EOF |
| Directory | `0x7006` | path, entry array, capacity | number of entries |
| Spawn | `0x7007` | executable callback, opaque argument, u64 tid output | status; direct child thread |
| Join | `0x7008` | tid, i64 exit-code output, timeout milliseconds | status |
| Cancel | `0x7009` | owned child/descendant tid | status; recursively requests termination |
| Tree | `0x700a` | entry array, capacity | current process's registered thread nodes |
| Sleep | `0x700b` | milliseconds | status |
| Exit | `0x700c` | signed exit code | does not return |
| GUI open | `0x700d` | kind: 1 video, 2 music, 3 local browser, 4 tree, 5 launcher | status; opens bounded GUI worker |
| Self | `0x700e` | u64 tid output | status |

Service bounds: write/read at most 4096 bytes per call; path at most 254 bytes;
allocation at most 16 MiB; directory/tree at most 64 entries; sleep/join at most
5000 ms. Larger file reads use offsets. Paths use the mounted kernel filesystem,
ASCII case-insensitive components and DOS backslashes (`C:\DOCS\WELCOME.TXT`).
There is no network socket, arbitrary device or port I/O API in kurazy v1.

## Thread-tree ownership

Spawn creates a real scheduler thread with its own stack and TEB in the parent's
process address space. The kernel publishes its process, entry and stack before
it can run. The callback receives its argument in RCX and must call
`kurazy_exit`; it must not return through an unspecified return address.

Each node records a unique tid and its creating thread's tid. Only the direct
parent can join a child. A join consumes that child's join right and returns its
signed exit code; a timeout preserves the right for retry. A parent may cancel
any descendant it owns, which recursively marks and terminates that entire
subtree. Peer threads and unrelated roots cannot cancel or join each other.
Cancelling a node gives cancelled threads exit code `KURAZY_E_CANCELLED` (-9).
Cancellation is completed at a syscall boundary or timer interrupt return for
that thread; a CPU-only user loop is preempted and observes pending cancellation.
The cancel request itself is asynchronous; join confirms completion.

An ordinary `kurazy_exit` automatically cancels descendants and gives them a
bounded opportunity to finish before releasing the parent's user stack. The
process launcher also enforces a 5-second total application deadline and a
bounded reaping grace period. Joined nodes retain ancestry and final exit status until process cleanup; never
use a stale snapshot as authority to operate on a thread. The kernel releases
joined scheduler references and clears the stored pointer before that scheduler
slot can be reused. Unjoined child references are released during process cleanup. Registration capacity is 128 nodes across
native processes and GUI workers.

GUI worker roots and children use the same ownership registry with a distinct
kernel-ID high bit and actual scheduler IDs. Their tree view includes desktop,
video, music and browser tasks. Native `kurazy_tree` deliberately shows only
its calling process; the GUI kernel view can inspect all registered tasks.

`THREAD64.EXE` exercises a parent, child and grandchild, successful joins,
unauthorized grandchild join rejection, cancellation and joined exit status.
`TREE64.EXE` runs two real sibling tasks concurrently. `CHECK64.EXE` exercises
bad pointers, wrong ABI size, a non-executable entry, invalid allocation,
missing files and an invalid GUI selector.

## Errors

| Code | Constant | Meaning |
|---|---|---|
| 0 | OK | successful operation |
| -1 | E_ARGUMENT | invalid size, count, selector or allocation base |
| -2 | E_ACCESS | inaccessible user pointer or non-executable entry |
| -3 | E_NOT_FOUND | missing file/thread |
| -4 | E_MEMORY | allocation failed |
| -5 | E_UNSUPPORTED | unavailable service or framebuffer |
| -6 | E_TIMEOUT | bounded join deadline expired |
| -7 | E_OWNER | caller does not own the requested thread operation |
| -8 | E_LIMIT | global tree registry full |
| -9 | E_CANCELLED | requested thread cancellation exit code |

Capabilities are implemented feature bits, not universal compatibility claims.
CONSOLE, MEMORY, FILES and THREADS are provided by the native kernel. GOP and
LOCAL_BROWSER are reported only with a boot GOP framebuffer. PCM_DECODE is
reserved and not asserted in this release. ShizukuGUI music presently plays
packaged note scores through the PIT/PC speaker; it does not implement USB,
HDA, AC97 or arbitrary PCM audio playback. The video player decodes the original
KV64 uncompressed truecolor format, not MP4/H.264. The browser implements an original bounded HTML renderer, local relative
navigation and plain HTTP/1.0 over native TCP. URLs use numeric IPv4 hosts and
optional ports, a four-second deadline and 64-KiB response cap. HTTP 200 and
CRLF headers are required; compressed/chunked responses are rejected. There
is no DNS, TLS, CSS or JavaScript. The default guest has no NIC; its self-test
uses an actual temporary loopback HTTP server. External HTTP browsing requires
the supported RTL8139 interface. See `samples/media/README.md` for exact contracts.

## Real-mode binding

The implemented DOS compatibility services are INT 21h: AH=02h character output,
09h `$`-terminated string, 30h DOS version, 40h console write (BX=1/2), and
4Ch process exit. Shizuku-specific AH=F0h queries actual mode: AX=16 and DX
contains the low CR0 bits. DOS MZ loading supports segment relocations and a
PSP for the packaged samples. Unsupported interrupts and functions are outside
the v1 contract; arbitrary DOS games, DOS extenders and device drivers are not
claimed compatible.

## Protected-mode binding

PE32/i386 preferred image base is `0x40000`, without imports. Entry runs cdecl
and returns its exit status in EAX. INT 80h EAX=1 writes ECX bytes from linear
EDX; EAX=2 snapshots into EDX the four-u32 struct `{bits, cr0, efer, cs}`;
EAX=3 is a cooperative yield placeholder because bridge IRQs remain masked.
There are no preemptive threads, DLL loader or full long-mode service parity
in this protected-mode binding. `HELLO32`, `MODE32` and `COUNT32` use exactly
this implemented subset. A future shared high-level API can extend these
bindings only through a version/capability change.
