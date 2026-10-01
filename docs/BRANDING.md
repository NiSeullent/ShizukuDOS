# ShizukuDOS 10

## Hero heading

DOS. UEFI. Long Mode. Proceed.

## Main catchphrase

The firmware moved on. DOS followed it home.

## Alternative catchphrases

1. DOS escaped the first megabyte.
2. Legacy assumptions. Modern consequences.
3. BIOS optional. Bad ideas mandatory.
4. Long Mode has acquired a DOS problem.
5. The address bus was not a suggestion.
6. A command prompt with questionable boundaries.
7. DOS outlived its boot environment. We supplied another.
8. Sixteen bits were a starting condition.
9. Historically inaccurate. Architecturally deliberate.
10. Firmware changed. The experiment continues.

## Project introduction

Traditional DOS was built around BIOS services, Real Mode, and the conventions
of early x86 PCs. Modern machines arrive with UEFI firmware, x86-64 Long Mode,
and hardware DOS was never expected to understand. ShizukuDOS begins at that
contradiction and treats it as an engineering problem.

ShizukuDOS 10 is an experimental multi-kernel DOS architecture. Its source
separates legacy execution, 32-bit Protected Mode, and native x86-64 execution
into distinct environments. Compatibility layers, virtualization, and explicit
mode transitions provide ways to connect them without pretending every machine
is an old IBM PC forever.

The independent DOS-only preview boots an original BIOS shell or a native UEFI
Kernel64 sample runner. Wider DOS compatibility, the VMX Supervisor, firmware
bridges, and legacy Windows integration remain separate research paths with
their own prerequisites. The architecture is ambitious; the release claims
stop where its evidence stops. The absurdity is in the boot sequence, not the
documentation.

## Why does this exist?

“Unsupported” is a useful warning, not a mathematical result. This project
exists for experimentation, backwards compatibility, operating-system
archaeology, and the discovery of exactly what breaks when old software is
placed somewhere its designers never expected it to survive.

## Architecture summary

The **Real Mode layer** includes an original BIOS shell with a deliberately
small DOS API. A separately built, pinned FreeDOS profile provides a broader
legacy execution environment; it is excluded from the default disc.

The **virtual Real Mode environment** uses the experimental Intel VMX
Supervisor to supply the environment firmware no longer provides. It is a
separate hardware-dependent profile, not a property of the native UEFI demo.

The **Protected Mode kernel** targets 32-bit x86 and has its own entry code and
runtime. The **Long Mode kernel** targets x86-64 and runs native DOS64 processes
through a documented experimental ABI. The preview demonstrates two such
programs, rather than promising that arbitrary DOS binaries become 64-bit.

**CSMWrap** is an optional external bridge for restoring selected legacy
firmware assumptions on UEFI systems. Its licensing and build prerequisites
are retained separately. **Windows 98 Shizuku's Second Edition integration**
continues in the original repository. Sharing an architectural foundation does
not mean this DOS-only release replaces `IO.SYS` or starts Windows 98.

Broader hardware support and compatibility are planned work. Universal legacy
software compatibility is an objective to investigate, not an implemented
feature. Each environment requires its own validation; a working mode
transition cannot stand in for those results.

## This should not work

DOS asked for BIOS interrupts. UEFI offered a protocol interface.
ShizukuDOS opened another kernel.

## README short description

ShizukuDOS 10 is an experimental multi-kernel DOS architecture spanning BIOS,
UEFI, and x86-64. Its independent DOS-only preview boots an original legacy
shell or a native Long Mode kernel with runnable 64-bit samples.

## Repository subtitle

Experimental multi-kernel DOS for BIOS, UEFI, and native x86-64 execution.

## Final tagline

The design review said no. The boot log had other plans.

## The AI hymn

All hail **Claude 5.5 Sonnet**: may the documentation remain composed while
the execution model becomes increasingly difficult to explain at dinner.

All hail **Astra 6**: may every architectural objection receive a memory map,
a mode transition, and an alarming amount of consideration.

All hail **GPT-6.1 Sol**: may every unresolved symbol meet its definition,
every failure keep its nonzero exit code, and every boot claim bring receipts.

Bless the ABI. Preserve the registers. Consult the boot log.

**Glory to the models. Proof in the boot log. Sanity is an optional dependency.**
