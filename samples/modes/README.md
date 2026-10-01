# Real Mode and Protected Mode examples

These six original programs accompany the native 64-bit utility collection.
They use actual CPU mode changes, with no instruction emulator and no firmware
BIOS service calls. `HELLO.EXE`, `MODE.EXE`, and `COUNT.EXE` are DOS MZ executables.
`HELLO32.EXE`, `MODE32.EXE`, and `COUNT32.EXE` are static PE32/i386 executables.

The UEFI loader reserves physical `0x10000..0x6ffff` before ExitBootServices.
Kernel64 copies a position-independent transition island there and adds
supervisor-only identity mappings. A legacy launch saves the kernel descriptor
tables, control registers, segment bases, stack and interrupt state. It enters
32-bit compatibility code, disables paging and Long Mode, and runs a Protected
Mode app with `CR0.PE=1`, `CR0.PG=0`, and `EFER.LMA=0`. For a DOS MZ app it also
switches to a 16-bit code segment, clears `CR0.PE`, and makes a real far jump.
The return path rebuilds Long Mode from the saved state and resumes the GUI.

The boot evidence includes observed CR0, EFER and CS values for every launch,
the application exit status, and successful return to Long Mode. `MODE.EXE`
and `MODE32.EXE` additionally read CR0 themselves. The MZ header contains an
actual relocation, which the DOS loader applies before entering the app.

This initial compatibility layer accepts only the six audited finite programs
through its public launcher. Legacy apps run privileged and synchronously with
interrupts masked. Arbitrary programs have no isolation or timeout here. This
is a scoped DOS console API, not a complete DOS installation or a DPMI host.
The BIOS ISO track separately retains its existing COM shell compatibility.

The MZ loader supplies a PSP, empty command tail, relocation processing, and
`DS=ES=PSP`. The program establishes its own data segment. Implemented INT 21h
calls are AH=02h (character output), 09h (`$`-terminated output), 30h (version
5.0), 40h (write console handles 1 and 2), and 4Ch (exit). INT 20h also exits.
Unsupported calls return AX=1 and Carry set; bounded console writes validate
the caller's conventional-memory range. The kurazy extension AH=F0h returns
AX=16 and the low CR0 bits in DX. Files, allocation services, graphics BIOS,
TSRs and hardware interrupts are outside this subset.

The static PE32 loader validates the machine, header, fixed image base
`0x40000`, section bounds and executable entry. It refuses imports and all
data directories; there are no Windows DLL dependencies. The entry is a
32-bit cdecl function returning its exit code in EAX. Software interrupt 80h
accepts EAX=1 (console: EDX buffer, ECX length), EAX=2 (write a 16-byte mode
snapshot at EDX: four u32 fields `bits, cr0, efer, cs`), or EAX=3 (successful
cooperative checkpoint; no rescheduling while the bridge masks interrupts).
Successful console writes return the byte count; query/checkpoint returns
zero; unsupported or invalid calls return `0xffffffff`. Applications must
preserve the cdecl callee-saved registers when returning.

Build offline with `python3 -B tools/build_mode_apps.py`. Host validation:

```sh
gcc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  samples/modes/test_formats.c -o build/modes/test_formats
./build/modes/test_formats
```
