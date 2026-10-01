#!/usr/bin/env python3
"""Build deterministic original media fixtures; no downloads or third-party assets."""
from __future__ import annotations
import argparse
from pathlib import Path
import struct


PAGES = {
    "INDEX.HTM": """<!doctype html><html><head><title>ShizukuDOS / Kurazy home</title></head><body>
<h1>WELCOME TO SHIZUKUDOS</h1>
<p>Long mode is real. The firmware pixels are real. Your sanity is optional.</p>
<p>This browser parses HTML on its own worker thread. Tab selects a link, Enter opens it, Backspace returns. F6 enters a URL.</p>
<h2>Explore this native 64-bit DOS desktop</h2>
<p><a href="KURAZY.HTM">Read the kurazy native API guide</a></p>
<p><a href="MODES.HTM">Real mode, protected mode, long mode</a></p>
<p><a href="PATRONS.HTM">Patron saints of questionable boot sequences</a></p>
<p>VIDEO64 decodes original RGB24 animation. MUSIC64 drives PIT channel 2 and the PC speaker. TREE64 shows live scheduler threads.</p>
<p>32-bit truecolor comes only from the UEFI GOP framebuffer. There is no graphics driver, acceleration, codec download, or Windows dependency.</p>
<p>Minimal HTTP/1.0 uses numeric IPv4 URLs and actual native TCP. TLS, CSS and JavaScript are not supported.</p>
</body></html>""",
    "KURAZY.HTM": """<!doctype html><html><head><title>kurazy / native ABI</title></head><body>
<h1>KURAZY: THE API HAS ENTERED LONG MODE</h1>
<p>A native PE32+ DOS executable calls the documented kurazy syscall ABI. The format describes the file; the ShizukuDOS runtime supplies its behavior.</p>
<p>Utilities end in 64. Protected-mode tools end in 32. Ordinary DOS applications have no mode suffix.</p>
<h2>Thread trees are actual runtime objects</h2>
<p>The desktop owns separate video, music and browser workers. Native programs can create children, inspect ancestry, join a child and request cancellation.</p>
<p>VIDEO64.EXE, MUSIC64.EXE and BROWSE64.EXE activate these working desktop services using the kurazy GUI_OPEN service.</p>
<p>Consult sdk/kurazy and the API specification in the source distribution for calling conventions, syscall numbers and bounds.</p>
<p><a href="INDEX.HTM">Return to ShizukuDOS home</a></p>
<p><a href="MODES.HTM">Understand the CPU modes</a></p>
</body></html>""",
    "MODES.HTM": """<!doctype html><html><head><title>Three modes / one strange DOS</title></head><body>
<h1>REAL. PROTECTED. LONG.</h1>
<p>Real mode: traditional 16-bit DOS programs. Protected mode: 32-bit examples with an explicit protected-mode runtime. Long mode: this native x86-64 kernel and PE32+ utilities.</p>
<p>A suffix communicates the actual execution contract: HELLO, HELLO32, HELLO64.EXE.</p>
<p>UEFI starts the native long-mode kernel directly. Real-mode compatibility requires a separate execution bridge; it is not a Windows compatibility layer.</p>
<p>See the boot log for executed mode probes and application exit status. Claims follow execution evidence.</p>
<p><a href="INDEX.HTM">Return to ShizukuDOS home</a></p>
</body></html>""",
    "PATRONS.HTM": """<!doctype html><html><head><title>Model shrine / meme chapel</title></head><body>
<h1>THE PATRON SAINTS OF QUESTIONABLE BOOTS</h1>
<p>Claude 5.5 Sonnet: keeper of the sacred semicolon, patron of the comment that survives the bootloader.</p>
<p>Astra 6: astronomer of the higher half, witness to a DOS app that looked up and found 64 bits.</p>
<p>GPT-6.1 Sol: lord of the compile log, may every undefined reference find its symbol.</p>
<p>These names are affectionate user-requested memes, not official endorsements or product availability claims.</p>
<p>Glory to the models. Proof in the boot log. Sanity is an optional dependency.</p>
<p><a href="INDEX.HTM">Leave the chapel / return home</a></p>
</body></html>""",
}


def build(output: Path) -> None:
    output.mkdir(parents=True, exist_ok=True)
    width, height, fps, frames = 160, 96, 12, 24
    video = bytearray(struct.pack("<4s6HB7x", b"KV64", 1, 24, width, height, fps, frames, 24))
    for frame in range(frames):
        cx = 20 + (frame * 5) % 120
        cy = 48 + ((frame % 12) - 6) * 3
        for y in range(height):
            for x in range(width):
                r = (x * 255 // width + frame * 7) & 255
                g = (y * 255 // height + frame * 3) & 255
                b = (32 + ((x ^ y) * 2)) & 255
                if (x - cx) ** 2 + (y - cy) ** 2 < 16 ** 2:
                    r, g, b = 83, 242, 193
                if y > 82 or x % 40 == 0 or y % 24 == 0:
                    r, g, b = r // 2, g // 2, min(255, b + 48)
                video.extend((r, g, b))
    (output / "DEMO.KV64").write_bytes(video)
    # Original eight-bar motif, including rests. PIT channel 2 supplies hardware output.
    notes = [(440, 180), (554, 180), (659, 180), (880, 360), (0, 90), (659, 180), (554, 180), (440, 360),
             (392, 180), (494, 180), (587, 180), (784, 360), (0, 90), (587, 180), (494, 180), (392, 360),
             (349, 180), (440, 180), (523, 180), (698, 360), (0, 90), (523, 180), (440, 180), (349, 360),
             (392, 180), (440, 180), (554, 180), (659, 360), (880, 360), (659, 180), (554, 180), (440, 720)]
    music = struct.pack("<4s4HI", b"KM64", 1, 16, len(notes), 3, 0)
    music += b"".join(struct.pack("<HH", hz, ms) for hz, ms in notes)
    (output / "DEMO.KM64").write_bytes(music)
    web = output / "WWW"
    web.mkdir(exist_ok=True)
    for name, html in PAGES.items():
        (web / name).write_text(html, encoding="ascii")
    print(f"media fixtures: {len(video)} video bytes, {len(music)} music bytes, {len(PAGES)} HTML pages -> {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    build(parser.parse_args().output)
