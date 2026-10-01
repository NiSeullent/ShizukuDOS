# ShizukuGUI media and browser

ShizukuGUI draws 32-bit truecolor pixels into the framebuffer left by UEFI GOP.
It never initializes a graphics driver, changes the firmware mode, or requires
Windows. The video, music and HTML document services have separate scheduled
kernel threads beneath the desktop in the kurazy thread tree.

F1 selects the browser, F2 video, F3 music, F4 the live thread view and F5 the
native utility launcher. Space pauses/resumes video or music. In the browser,
Tab selects a link, Enter opens it, Backspace returns to the previous page and
the up/down arrows scroll. F6 opens an empty address field; type a packaged
`C:\WWW\INDEX.HTM` path or numeric IPv4 HTTP URL and press Enter. In the launcher, Tab or arrows select a native EXE
and Enter executes it. F8 runs the explicit acceptance suite after the desktop is interactive.
F10 ends the desktop and requests guest shutdown.
Keyboard input currently requires an i8042 PS/2 keyboard or firmware emulation;
USB HID and mouse input are not implemented in this standalone desktop.

The browser is an original bounded HTML parser for packaged local documents
and plain HTTP/1.0 responses over the native TCP stack.
It renders titles, text, paragraphs, headings, lists, links and basic entities;
it hides script/style content and rejects parent-directory traversal. It has
working document navigation and history. HTTP addresses use numeric IPv4 hosts
and optional ports, for example `http://10.0.2.2:8000/INDEX.HTM`. There is no DNS,
TLS, CSS or JavaScript support. Transactions have a four-second bound and a
64-KiB response cap; compressed and chunked responses are rejected. The default
isolated test guest has no NIC and verifies actual TCP/HTTP through a temporary
loopback server. Network browsing requires an RTL8139 interface supported by the
existing native network stack; local pages remain available without networking.

VIDEO64 plays the original `DEMO.KV64` fixture. KV64 version 1 is uncompressed
RGB24 frame animation, not a general-purpose compressed video decoder. Every
frame is decoded from the ISO initrd and scaled in software to the player pane.
The little-endian 24-byte header contains:

| Offset | Field |
|---|---|
| 0 | Four bytes `KV64` |
| 4 | uint16 version = 1 |
| 6 | uint16 header size = 24 |
| 8, 10 | uint16 width, height (1..512) |
| 12, 14 | uint16 frames per second (1..60), frame count (1..600) |
| 16 | uint8 bits per pixel = 24 |
| 17..23 | Reserved, emitted as zero |
| 24 | Consecutive row-major RGB byte triples; frames have no padding |

The parser requires the exact payload size and caps it at 32 MiB. The supplied
fixture is 160x96, 12 fps, 24 frames and contains no imported media.

MUSIC64 plays the original `DEMO.KM64` tune through the physical PC speaker,
using PIT channel 2 and the speaker gate. It plays actual timed notes and rests;
its note/progress state follows the scheduled sequencer. It does not claim PCM,
WAV, MP3 or modern sound-card support. Some QEMU builds omit PC-speaker audio
output: the PIT commands still execute, but audible host playback then requires
a QEMU build that supports `pcspk-audiodev` and an audio backend.

KM64 version 1 has a 16-byte little-endian header: bytes 0..3 `KM64`, uint16
version = 1 at 4, uint16 header size = 16 at 6, uint16 note count (1..1024) at 8,
uint16 repeat count (1..100) at 10, and uint32 reserved = 0 at 12. Each following
four-byte note is uint16 frequency in Hz (0 = rest, otherwise 20..20000) then
uint16 duration in milliseconds (1..10000). The parser requires an exact size.

`build.py --output <ignored-build-directory>` regenerates all fixtures offline.
`test_media.c` exercises the real fixture decoders plus truncated media, invalid
dimensions, missing duration, HTML entities, script suppression, navigation and
path rejection. The guest self-test also checks two distinct rendered video
frames, reads the actual GOP framebuffer back, follows a packaged link and its
history, fetches HTML from a real temporary TCP/HTTP loopback server, programs
actual PIT notes and verifies scheduled workers. It leaves
the desktop open for independent keyboard and screenshot verification.
