/* SPDX-License-Identifier: GPL-2.0-only
 * ShizukuGUI: original GOP-only, 32-bit software desktop. No display device is
 * probed or programmed: the firmware framebuffer is the sole scanout target.
 * A real RGB24 animation decoder, PIT-speaker music sequencer and bounded HTML
 * document browser run as child kernel threads of the interactive desktop.
 */
#include "shizukugui.h"
#include "kurazy_media.h"
#include "gfx.h"
#include "fs.h"
#include "pci.h"
#include "dos64.h"
#include "net.h"
#include "kurazy.h"
#include "boot_console.h"

#define BG 0x0b2028u
#define PANEL 0x142e39u
#define INK 0xd4f4e9u
#define MUTED 0x92b3bbu
#define TEAL 0x53f2c1u
#define PURPLE 0xbca2ffu
#define YELLOW 0xffd67bu
typedef struct { int x, y, w, h; } rect_t;
typedef struct {
    int ready, running, quit, failed;
    uint32_t width, height, pitch, rgbx;
    volatile uint32_t *front;
    uint32_t *back;
    kmutex_t lock;
    ksem_t browser_sem;
    thread_t *video_thread, *music_thread, *browser_thread, *desktop_thread;
    kz_video_t video;
    kz_music_t music;
    kz_html_t page;
    char url[160], request[160], history[8][160], message[96];
    unsigned history_count, request_id, complete_id, link, scroll, selected_app;
    unsigned frame, frames_decoded, note, notes_played, hz;
    uint64_t music_elapsed, video_epoch;
    int video_playing, music_playing, dirty, address_edit;
    char address[160];
    unsigned focus, render_seq;
    rect_t browser, video_rect, music_rect;
} desktop_t;
static desktop_t g;

/* The native runtime supplies this callback; panes never invoke it recursively. */
extern int dos64_launch_named(const char *name);
extern unsigned shizukudos_acceptance_run(void);

static const char *apps[] = {
    "HELLO64.EXE", "MEM64.EXE", "CPU64.EXE", "TREE64.EXE", "DIR64.EXE",
    "VIDEO64.EXE", "MUSIC64.EXE", "BROWSE64.EXE", "TIME64.EXE", "TYPE64.EXE",
    "HASH64.EXE", "INFO64.EXE", "THREAD64.EXE", "CHECK64.EXE",
    "HELLO.EXE", "MODE.EXE", "COUNT.EXE", "HELLO32.EXE", "MODE32.EXE", "COUNT32.EXE"
};
#define APP_COUNT (sizeof apps / sizeof apps[0])
static void strcopy(char *out, const char *s, unsigned cap)
{ unsigned i = 0; while (i + 1 < cap && s[i]) { out[i] = s[i]; ++i; } out[i] = 0; }
static char *number(char *s, uint64_t n)
{
    char b[24]; unsigned i = 0;
    do { b[i++] = (char)('0' + n % 10); n /= 10; } while (n);
    while (i) *s++ = b[--i];
    *s = 0; return s;
}
static void text(int x, int y, const char *s, uint32_t color)
{
    uint16_t u[160]; unsigned n = 0;
    while (s[n] && n < 159) { u[n] = (uint8_t)s[n]; ++n; }
    gfx_text(g.back, (int)g.width, (int)g.width, (int)g.height, x, y, u, n, color, 0, 0, (int)g.width, (int)g.height);
}
static void clipped_text(int x, int y, const char *s, uint32_t color, rect_t r)
{
    uint16_t u[160]; unsigned n = 0;
    while (s[n] && n < 159) { u[n] = (uint8_t)s[n]; ++n; }
    gfx_text(g.back, (int)g.width, (int)g.width, (int)g.height, x, y, u, n, color, r.x, r.y, r.x + r.w, r.y + r.h);
}
static void fill(int x, int y, int w, int h, uint32_t color)
{
    int xx, yy, endx = x + w, endy = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (endx > (int)g.width) endx = (int)g.width;
    if (endy > (int)g.height) endy = (int)g.height;
    for (yy = y; yy < endy; ++yy)
        for (xx = x; xx < endx; ++xx) g.back[(uint64_t)yy * g.width + (unsigned)xx] = color;
}
static void window(rect_t r, const char *title, unsigned pane)
{
    fill(r.x + 5, r.y + 5, r.w, r.h, 0x031117);
    fill(r.x, r.y, r.w, r.h, PANEL);
    fill(r.x, r.y, r.w, 30, g.focus == pane ? 0x245e62 : 0x1e424a);
    fill(r.x, r.y, 3, r.h, g.focus == pane ? TEAL : 0x315760);
    text(r.x + 12, r.y + 7, title, INK);
    fill(r.x + r.w - 20, r.y + 11, 7, 7, g.focus == pane ? TEAL : MUTED);
}
static void present(void)
{
    uint32_t y, x;
    for (y = 0; y < g.height; ++y) {
        volatile uint32_t *dst = g.front + (uint64_t)y * g.pitch;
        const uint32_t *src = g.back + (uint64_t)y * g.width;
        if (g.rgbx) {
            for (x = 0; x < g.width; ++x) {
                uint32_t c = src[x]; dst[x] = ((c & 255) << 16) | (c & 0xff00) | ((c >> 16) & 255);
            }
        } else {
            uint64_t count = g.width;
            __asm__ volatile("rep movsl" : "+D"(dst), "+S"(src), "+c"(count) :: "memory");
        }
    }
}
static uint32_t checksum(rect_t r)
{
    uint32_t hash = 2166136261u; int x, y;
    for (y = r.y; y < r.y + r.h && y < (int)g.height; ++y)
        for (x = r.x; x < r.x + r.w && x < (int)g.width; ++x) {
            if (x >= 0 && y >= 0) { hash ^= g.back[(uint64_t)y * g.width + (unsigned)x]; hash *= 16777619u; }
        }
    return hash;
}
static uint32_t front_checksum(void)
{
    uint32_t hash = 2166136261u, x, y;
    for (y = 0; y < g.height; ++y)
        for (x = 0; x < g.width; ++x) {
            uint32_t c = g.front[(uint64_t)y * g.pitch + x];
            if (g.rgbx) c = ((c & 255) << 16) | (c & 0xff00) | ((c >> 16) & 255);
            hash ^= c & 0xffffff; hash *= 16777619u;
        }
    return hash;
}
static void render_browser(void)
{
    rect_t r = g.browser, content; unsigned i, visible;
    window(r, "BROWSE64 / original HTML browser", 3);
    fill(r.x + 12, r.y + 40, r.w - 24, 25, 0x0b222c);
    clipped_text(r.x + 20, r.y + 45, g.address_edit ? g.address : g.url, TEAL, r);
    clipped_text(r.x + 16, r.y + 78, g.page.title, YELLOW, r);
    content.x = r.x + 16; content.y = r.y + 105; content.w = r.w - 32; content.h = r.h - 196;
    visible = content.h > 0 ? (unsigned)content.h / 19 : 0;
    for (i = 0; i < visible && i + g.scroll < g.page.line_count; ++i)
        clipped_text(content.x, content.y + (int)i * 19, g.page.lines[i + g.scroll], INK, content);
    clipped_text(r.x + 16, r.y + r.h - 88, g.message, MUTED, r);
    if (g.page.link_count) {
        fill(r.x + 12, r.y + r.h - 65, r.w - 24, 25, 0x21444b);
        clipped_text(r.x + 20, r.y + r.h - 60, g.page.links[g.link].label, TEAL, r);
    }
    clipped_text(r.x + 16, r.y + r.h - 31, "Tab: link  Enter: open  Back: history  F6: URL", MUTED, r);
}
static rect_t video_pixels(void)
{
    rect_t r = g.video_rect;
    r.x += 12; r.y += 43; r.w -= 24; r.h -= 95;
    return r;
}
static void render_video(void)
{
    rect_t r = g.video_rect, pixels = video_pixels(); int x, y; char s[96], *p;
    window(r, "VIDEO64 / RGB24 motion", 1);
    if (g.video.pixels && pixels.w > 0 && pixels.h > 0) {
        for (y = 0; y < pixels.h; ++y)
            for (x = 0; x < pixels.w; ++x) {
                const unsigned sx = (unsigned)x * g.video.width / (unsigned)pixels.w;
                const unsigned sy = (unsigned)y * g.video.height / (unsigned)pixels.h;
                int px = pixels.x + x, py = pixels.y + y;
                if (px >= 0 && py >= 0 && px < (int)g.width && py < (int)g.height)
                    g.back[(uint64_t)py * g.width + (unsigned)px] = kz_video_pixel(&g.video, g.frame, sx, sy);
            }
    } else clipped_text(r.x + 15, r.y + 60, "No valid C:\\MEDIA\\DEMO.KV64", YELLOW, r);
    strcopy(s, g.video_playing ? "PLAY  frame " : "PAUSE frame ", sizeof s);
    p = s + strlen(s); p = number(p, g.frame); *p++ = '/'; p = number(p, g.video.frames);
    *p++ = ' '; *p++ = '@'; *p++ = ' '; p = number(p, g.video.fps); strcopy(p, " fps", 16);
    clipped_text(r.x + 16, r.y + r.h - 43, s, TEAL, r);
    clipped_text(r.x + 16, r.y + r.h - 23, "Space: pause / resume", MUTED, r);
}
static void render_music(void)
{
    rect_t r = g.music_rect; char s[96], *p; int i, barwidth, chart_h;
    window(r, "MUSIC64 / PC speaker sequencer", 2);
    clipped_text(r.x + 16, r.y + 45, "C:\\MEDIA\\DEMO.KM64", MUTED, r);
    chart_h = r.h - 150; if (chart_h > 80) chart_h = 80; if (chart_h < 12) chart_h = 12;
    barwidth = (r.w - 32) / 32;
    for (i = 0; i < 32 && g.music.notes; ++i) {
        unsigned hz, ms; int h;
        if (kz_music_note(&g.music, (unsigned)i % g.music.count, &hz, &ms)) break;
        h = (int)(hz * (unsigned)chart_h / 1000); if (h > chart_h) h = chart_h;
        fill(r.x + 16 + i * barwidth, r.y + 78 + chart_h - h, barwidth - 2, h,
             (unsigned)i == g.note ? TEAL : 0x427878);
    }
    strcopy(s, g.music_playing ? "PLAY  note " : "PAUSE note ", sizeof s);
    p = s + strlen(s); p = number(p, g.note); strcopy(p, " / ", 4); p += 3; p = number(p, g.music.count);
    strcopy(p, "   ", 4); p += 3; p = number(p, g.hz); strcopy(p, " Hz", 8);
    clipped_text(r.x + 16, r.y + r.h - 67, s, TEAL, r);
    strcopy(s, "Output: PIT channel 2 / physical speaker", sizeof s);
    clipped_text(r.x + 16, r.y + r.h - 44, s, MUTED, r);
    clipped_text(r.x + 16, r.y + r.h - 23, "Space: play / pause", MUTED, r);
}
static void render_tree(void)
{
    rect_t r = { 54, 110, (int)g.width - 108, (int)g.height - 188 };
    unsigned i, count = 0, node_count; int row = r.y + 78; char s[128], *p;
    kurazy_thread_info nodes[128];
    node_count = kurazy_tree_snapshot(nodes, 128, -1);
    window(r, "TREE64 / live scheduler ancestry", 4);
    text(r.x + 18, r.y + 44, "desktop -> video / music / browser; process -> thread", TEAL);
    for (i = 0; ; ++i) {
        uint64_t f = irq_save(); thread_t *t = thread_slot(i);
        if (!t) { irq_restore(f); break; }
        if (t->state == TS_FREE) { irq_restore(f); continue; }
        ++count;
        strcopy(s, (t == g.video_thread || t == g.music_thread || t == g.browser_thread) ? "  |-- " : "+-- ", sizeof s);
        p = s + strlen(s); strcopy(p, t->name, 20); p += strlen(p);
        strcopy(p, "  id=", 8); p += 5; p = number(p, t->id);
        strcopy(p, "  state=", 10); p += 8; p = number(p, t->state);
        strcopy(p, "  ticks=", 10); p += 8; p = number(p, t->run_ticks);
        if (t->proc) { strcopy(p, " pid=", 8); p += 5; p = number(p, (unsigned)t->proc->pid); }
        else {
            unsigned node; uint64_t id = kurazy_tree_kernel_id(t);
            for (node = 0; node < node_count && node < 128; ++node) if (nodes[node].tid == id && nodes[node].parent_tid) {
                strcopy(p, " parent=", 10); p += 8; p = number(p, nodes[node].parent_tid & ~(1ull << 63)); break;
            }
        }
        irq_restore(f);
        if (row + 19 < r.y + r.h - 32) { clipped_text(r.x + 18, row, s, INK, r); row += 21; }
    }
    strcopy(s, "Scheduler slots active: ", sizeof s); number(s + strlen(s), count);
    clipped_text(r.x + 18, r.y + r.h - 28, s, TEAL, r);
}
static void render_launcher(void)
{
    rect_t r = { 84, 130, (int)g.width - 168, (int)g.height - 225 };
    unsigned i, visible = r.h > 116 ? (unsigned)(r.h - 116) / 28 : 1;
    unsigned first = g.selected_app >= visible ? g.selected_app - visible + 1 : 0;
    int row = r.y + 72;
    window(r, "Native applications / real + protected + long mode", 5);
    text(r.x + 20, r.y + 43, "Tab or arrows: select    Enter: execute", TEAL);
    for (i = first; i < APP_COUNT && i < first + visible; ++i, row += 28) {
        if (i == g.selected_app) fill(r.x + 14, row - 4, r.w - 28, 26, 0x28585e);
        clipped_text(r.x + 24, row, apps[i], i == g.selected_app ? TEAL : INK, r);
    }
    clipped_text(r.x + 20, r.y + r.h - 28, g.message, YELLOW, r);
}
static void render(void)
{
    char s[96], rendered_url[160]; char *p; int x, y;
    unsigned rendered_pane, rendered_seq;
    mutex_lock(&g.lock);
    fill(0, 0, (int)g.width, (int)g.height, BG);
    for (y = 85; y < (int)g.height - 44; y += 32)
        for (x = 8; x < (int)g.width; x += 32) fill(x, y, 1, 1, 0x22505a);
    text(24, 19, "ShizukuGUI", TEAL);
    text(24, 43, "64-bit DOS. Firmware pixels. Kurazy possibilities.", MUTED);
    strcopy(s, "GOP / ", sizeof s); p = number(s + strlen(s), g.width); *p++ = 'x'; p = number(p, g.height);
    strcopy(p, " / 32-bit truecolor", 24);
    text((int)g.width - (int)strlen(s) * 8 - 24, 24, s, TEAL);
    render_browser(); render_video(); render_music();
    if (g.focus == 4) render_tree();
    if (g.focus == 5) render_launcher();
    fill(0, (int)g.height - 40, (int)g.width, 40, 0x173b43);
    text(18, (int)g.height - 27, "F1 Web  F2 Video  F3 Music  F4 Tree  F5 Apps", TEAL);
    text((int)g.width - 250, (int)g.height - 27, "F8: tests  F10: shutdown", MUTED);
    g.dirty = 0;
    present();
    rendered_pane = g.focus; rendered_seq = ++g.render_seq;
    strcopy(rendered_url, g.url, sizeof rendered_url);
    mutex_unlock(&g.lock);
    kprintf("SHZGUI PRESENT pane=%u seq=%u url=%s\n", rendered_pane, rendered_seq, rendered_url);
}

static const uint8_t *read_fixture(const char *path, uint64_t *n)
{
    fsnode_t *f = fs_lookup(path);
    if (!f || f->is_dir || f->backing != FSB_RAM || !f->data) return 0;
    *n = f->size; return f->data;
}
static void speaker(unsigned hz)
{
    uint64_t flags = irq_save();
    if (!hz) k_outb(0x61, k_inb(0x61) & (uint8_t)~3u);
    else {
        unsigned divisor = 1193182u / hz;
        k_outb(0x43, 0xb6); k_outb(0x42, (uint8_t)divisor); k_outb(0x42, (uint8_t)(divisor >> 8));
        k_outb(0x61, k_inb(0x61) | 3u);
    }
    irq_restore(flags);
}
static void video_worker(void *unused)
{
    (void)unused;
    while (g.running) {
        uint64_t now = ticks_now();
        mutex_lock(&g.lock);
        if (g.video_playing && g.video.frames) {
            unsigned frame = (unsigned)((now - g.video_epoch) * g.video.fps / 1000) % g.video.frames;
            if (frame != g.frame) { g.frame = frame; ++g.frames_decoded; g.dirty = 1; }
        }
        mutex_unlock(&g.lock);
        thread_sleep_ms(10);
    }
    thread_exit(0);
}
static void music_worker(void *unused)
{
    uint64_t last = ticks_now(), note_ms = 0; unsigned active_hz = 0, duration = 0, hz = 0, repeats = 0;
    int was_playing = 0;
    (void)unused;
    while (g.running) {
        uint64_t now = ticks_now(), delta = now - last; last = now;
        mutex_lock(&g.lock);
        if (g.music_playing && g.music.notes) {
            if (!was_playing) {
                kz_music_note(&g.music, g.note, &hz, &duration); active_hz = hz; speaker(hz);
                ++g.notes_played; g.hz = hz; was_playing = 1; g.dirty = 1;
                kprintf("SHZGUI SPEAKER note=%u frequency=%u duration_ms=%u\n", g.note, hz, duration);
            }
            note_ms += delta; g.music_elapsed += delta;
            if (note_ms >= duration) {
                note_ms -= duration;
                if (++g.note >= g.music.count) {
                    g.note = 0;
                    if (++repeats >= g.music.repeats) { g.music_playing = 0; repeats = 0; }
                }
                if (g.music_playing) {
                    kz_music_note(&g.music, g.note, &hz, &duration); active_hz = hz; speaker(hz);
                    ++g.notes_played; g.hz = hz;
                    kprintf("SHZGUI SPEAKER note=%u frequency=%u duration_ms=%u\n", g.note, hz, duration);
                }
                g.dirty = 1;
            }
        }
        if (!g.music_playing && was_playing) { speaker(0); active_hz = 0; g.hz = 0; was_playing = 0; g.dirty = 1; }
        mutex_unlock(&g.lock);
        (void)active_hz;
        thread_sleep_ms(5);
    }
    speaker(0); thread_exit(0);
}
static int http_fetch(const char *url, uint8_t *response, size_t *size)
{
    uint32_t ip; unsigned port, used = 0; char path[160], request[384], *p;
    sock_t *sock; tcb_t *tcp; int32_t error = 0; int result = -1; uint64_t end;
    if (kz_http_url(url, &ip, &port, path) || net_ensure_init()) return -1;
    strcopy(request, "GET ", sizeof request); p = request + strlen(request);
    strcopy(p, path, 160); p += strlen(p);
    strcopy(p, " HTTP/1.0\r\nHost: ", 24); p += strlen(p);
    p = number(p, (ip >> 24) & 255); *p++ = '.'; p = number(p, (ip >> 16) & 255); *p++ = '.';
    p = number(p, (ip >> 8) & 255); *p++ = '.'; p = number(p, ip & 255);
    strcopy(p, "\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n", 64);
    net_lock(); sock = sock_new(SK_STREAM, 1);
    if (!sock) { net_unlock(); return -1; }
    tcp = tcp_connect(sock, ip, (uint16_t)port, &error); end = net_now() + 4000;
    if (tcp) {
        while (tcp->state == TCPS_SYN_SENT && !tcp->err && net_now() < end) net_sleep(20);
        if (tcp_can_write(tcp) && bq_write(&tcp->sndq, request, (uint32_t)strlen(request)) == strlen(request)) {
            tcp_output(tcp);
            while (net_now() < end && !tcp->err && g.running) {
                unsigned count = tcp->rcvq.len;
                if (count) {
                    if (count > 65536u - used) break;
                    bq_peek(&tcp->rcvq, 0, response + used, count); bq_drop(&tcp->rcvq, count);
                    used += count; tcp_after_read(tcp);
                }
                if (tcp->rcvd_fin) { result = 0; break; }
                if (used == 65536) break;
                net_sleep(20);
            }
        }
    }
    sock_close_kernel(sock); net_unlock(); *size = used;
    kprintf("SHZGUI HTTP fetch=%s bytes=%u status=%d\n", url, used, result);
    return result;
}
static void http_test_server(void *arg)
{
    static const char reply[] = "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
        "<title>HTTP loopback / real TCP</title><h1>HTTP WORKS</h1><p>Original browser over actual TCP, without a NIC.</p>"
        "<p><a href='/NEXT.HTM'>A relative HTTP link</a></p>";
    sock_t *listener = arg, *accepted = 0; tcb_t *tcp = 0; uint64_t end = net_now() + 4000;
    int result = -1; char method[4];
    net_lock();
    while (!listener->acc_count && net_now() < end) net_sleep(20);
    if (listener->acc_count) {
        tcp = tcp_accept_dequeue(listener); accepted = sock_new(SK_STREAM, 1);
        if (accepted) {
            accepted->tcb = tcp; tcp->sock = accepted; accepted->connected = 1; accepted->bound = 1;
            accepted->lip = tcp->lip; accepted->lport = tcp->lport; accepted->rip = tcp->rip; accepted->rport = tcp->rport;
            while (tcp->rcvq.len < 4 && !tcp->err && net_now() < end) net_sleep(20);
            if (tcp->rcvq.len >= 4) {
                bq_peek(&tcp->rcvq, 0, method, 4); bq_drop(&tcp->rcvq, tcp->rcvq.len); tcp_after_read(tcp);
                if (!memcmp(method, "GET ", 4) && bq_write(&tcp->sndq, reply, sizeof reply - 1) == sizeof reply - 1) {
                    tcp_output(tcp); tcp_shutdown_write(tcp); result = 0;
                }
            }
            sock_close_kernel(accepted);
        } else tcp_abort(tcp, 1);
    }
    sock_close_kernel(listener); net_unlock(); thread_exit(result);
}
static void browser_worker(void *unused)
{
    kz_html_t *page = kmalloc(sizeof *page);
    (void)unused;
    if (!page) { g.failed = 1; thread_exit(-1); }
    while (g.running) {
        char path[160]; unsigned id; const uint8_t *bytes; uint8_t *response = 0; uint64_t n = 0; int result;
        sem_wait(&g.browser_sem);
        if (!g.running) break;
        mutex_lock(&g.lock); strcopy(path, g.request, sizeof path); id = g.request_id; mutex_unlock(&g.lock);
        if (!strncmp(path, "http://", 7)) {
            size_t received = 0, body_size = 0;
            response = kmalloc(65536); bytes = 0;
            if (response && !http_fetch(path, response, &received) && !kz_http_body(response, received, &bytes, &body_size)) n = body_size;
        } else bytes = read_fixture(path, &n);
        result = !bytes || kz_html_parse(page, bytes, (size_t)n);
        mutex_lock(&g.lock);
        if (!result) {
            memcpy(&g.page, page, sizeof *page); strcopy(g.url, path, sizeof g.url); g.link = 0; g.scroll = 0;
            strcopy(g.message, response ? "Loaded over HTTP/1.0 / actual TCP." : "Loaded packaged HTML / native bounded parser.", sizeof g.message);
        } else strcopy(g.message, "Document missing or rejected by bounded HTML parser.", sizeof g.message);
        g.complete_id = id; g.dirty = 1;
        mutex_unlock(&g.lock);
        if (!result) kprintf("SHZGUI BROWSER path=%s title=%s\n", path, page->title);
        kfree(response);
    }
    kfree(page); thread_exit(0);
}
static unsigned browser_request(const char *path, int history)
{
    unsigned id;
    mutex_lock(&g.lock);
    if (history && g.url[0] && g.history_count < 8) strcopy(g.history[g.history_count++], g.url, 160);
    strcopy(g.request, path, sizeof g.request); id = ++g.request_id;
    mutex_unlock(&g.lock); sem_post(&g.browser_sem); return id;
}
static int browser_wait(unsigned id)
{
    uint64_t end = ticks_now() + 5000;
    while (g.complete_id < id && ticks_now() < end) thread_sleep_ms(5);
    return g.complete_id >= id ? 0 : -1;
}
static void key(unsigned key)
{
    char path[160]; const char *launch = 0; int navigate = 0, acceptance = 0;
    mutex_lock(&g.lock);
    if (key == 0x42) {
        acceptance = 1; g.address_edit = 0;
        strcopy(g.message, "Running full CPU-mode, native application and media acceptance tests...", sizeof g.message);
    } else if (g.address_edit) {
        if (key == 0x01) g.address_edit = 0;
        else if (key == 0x0e) { size_t len = strlen(g.address); if (len) g.address[len - 1] = 0; }
        else if (key == 0x1c) { strcopy(path, g.address, sizeof path); navigate = 2; g.address_edit = 0; }
    } else if (key == 0x40) {
        g.focus = 3; g.address_edit = 1; g.address[0] = 0;
    } else if (key >= 0x3b && key <= 0x3f) {
        static const unsigned panes[] = { 3, 1, 2, 4, 5 };
        g.focus = panes[key - 0x3b];
    } else if (key == 0x44) g.quit = 1;
    else if (key == 1) g.focus = 3;
    else if (key == 0x39) {
        if (g.focus == 1) { g.video_playing = !g.video_playing; g.video_epoch = ticks_now() - g.frame * 1000 / g.video.fps; }
        if (g.focus == 2) g.music_playing = !g.music_playing;
    } else if (key == 0x0f) {
        if (g.focus == 3 && g.page.link_count) g.link = (g.link + 1) % g.page.link_count;
        if (g.focus == 5) g.selected_app = (g.selected_app + 1) % APP_COUNT;
    } else if (key == 0x48) {
        if (g.focus == 3 && g.scroll) --g.scroll;
        if (g.focus == 5) g.selected_app = (g.selected_app + APP_COUNT - 1) % APP_COUNT;
    } else if (key == 0x50) {
        if (g.focus == 3 && g.scroll + 1 < g.page.line_count) ++g.scroll;
        if (g.focus == 5) g.selected_app = (g.selected_app + 1) % APP_COUNT;
    } else if (key == 0x0e && g.focus == 3 && g.history_count) {
        strcopy(path, g.history[--g.history_count], sizeof path); navigate = 1;
    } else if (key == 0x1c) {
        if (g.focus == 3 && g.page.link_count) {
            int result = kz_html_resolve(g.url, g.page.links[g.link].href, path);
            if (!result) navigate = 2;
            else strcopy(g.message, result == -2 ? "Only numeric IPv4 HTTP and packaged local URLs are supported." : "Unsafe or unsupported URL rejected.", sizeof g.message);
        }
        if (g.focus == 5) launch = apps[g.selected_app];
    }
    g.dirty = 1;
    mutex_unlock(&g.lock);
    if (acceptance) {
        unsigned failed;
        render();
        failed = shizukudos_acceptance_run();
        mutex_lock(&g.lock);
        strcopy(g.message, failed ? "Acceptance tests FAILED. See the diagnostic log." : "Acceptance tests PASSED: CPU modes, native EXEs, GOP, media and HTTP.", sizeof g.message);
        g.dirty = 1; mutex_unlock(&g.lock);
        render();
        kprintf("SHZGUI ACCEPTANCE COMPLETE result=%s\n", failed ? "FAIL" : "PASS");
    }
    if (navigate) browser_request(path, navigate == 2);
    if (launch) {
        int result = dos64_launch_named(launch);
        mutex_lock(&g.lock);
        strcopy(g.message, result ? "Native application reported a failure; see serial log." : "Native application returned success; see serial log.", sizeof g.message);
        g.dirty = 1; mutex_unlock(&g.lock);
        kprintf("SHZGUI LAUNCH %s result=%d\n", launch, result);
    }
    kprintf("SHZGUI INPUT scan=%02x pane=%u\n", key, g.focus);
}
/* Poll set-1 keyboard bytes; configure only the keyboard i8042 port. USB-only
 * physical keyboards need firmware PS/2 emulation, documented for this release. */
static int controller_ready(void)
{
    unsigned i; for (i = 0; i < 100000; ++i) if (!(k_inb(0x64) & 2)) return 0; return -1;
}
static int keyboard_init(void)
{
    unsigned i; uint8_t config = 0x40;
    if (k_inb(0x64) == 0xff) return -1;
    for (i = 0; i < 64 && (k_inb(0x64) & 1); ++i) (void)k_inb(0x60);
    if (controller_ready()) return -1;
    k_outb(0x64, 0x20);
    for (i = 0; i < 100000; ++i) if (k_inb(0x64) & 1) { config = k_inb(0x60); break; }
    if (i == 100000) return -1;
    config = (uint8_t)((config | 0x40) & ~0x11u); /* translation; no IRQ1; keyboard clock on */
    if (controller_ready()) return -1;
    k_outb(0x64, 0x60);
    if (controller_ready()) return -1;
    k_outb(0x60, config);
    if (controller_ready()) return -1;
    k_outb(0x64, 0xae);
    return 0;
}
static void poll_keyboard(void)
{
    static int shift; unsigned i;
    static const char ascii[58] = {
        0,0,'1','2','3','4','5','6','7','8','9','0','-','=',0,0,
        'q','w','e','r','t','y','u','i','o','p','[',']',0,0,
        'a','s','d','f','g','h','j','k','l',';','\'', '`',0,'\\',
        'z','x','c','v','b','n','m',',','.','/',0,'*',0,' '
    };
    for (i = 0; i < 32; ++i) {
        uint8_t status = k_inb(0x64), scan;
        if (!(status & 1)) break;
        scan = k_inb(0x60);
        if (scan == 0x2a || scan == 0x36) { shift = 1; continue; }
        if (scan == 0xaa || scan == 0xb6) { shift = 0; continue; }
        if ((status & 0x20) || scan == 0xe0 || scan == 0xe1 || scan >= 0x80) continue;
        if (g.address_edit && scan < sizeof ascii && ascii[scan]) {
            char c = ascii[scan]; size_t len;
            if (shift && c >= 'a' && c <= 'z') c -= 32;
            if (shift && c == ';') c = ':';
            mutex_lock(&g.lock); len = strlen(g.address);
            if (len + 1 < sizeof g.address) { g.address[len] = c; g.address[len + 1] = 0; }
            g.dirty = 1; mutex_unlock(&g.lock); continue;
        }
        key(scan);
    }
}

int shizukugui_open(unsigned kind)
{
    if (kind < 1 || kind > 5) return -1;
    if (g.ready) mutex_lock(&g.lock);
    g.focus = kind; g.dirty = 1;
    if (g.ready) mutex_unlock(&g.lock);
    kprintf("SHZGUI OPEN pane=%u\n", kind);
    return 0;
}
int shizukugui_init(void)
{
    k64_boot_fb_t fb; const uint8_t *bytes; uint64_t n = 0; int right_x, usable;
    if (g.ready) return 0;
    if (k64_boot_framebuffer(&fb) || fb.width < 640 || fb.height < 480 || fb.width > 4096 || fb.height > 4096) {
        kprintf("SHZGUI unavailable: a 640x480..4096x4096 32-bit GOP framebuffer is required\n"); return -1;
    }
    g.front = k64_boot_framebuffer_map();
    g.back = gfx_pages_alloc((uint64_t)fb.width * fb.height * 4);
    if (!g.front || !g.back) return -1;
    g.width = fb.width; g.height = fb.height; g.pitch = fb.pitch / 4; g.rgbx = fb.format == SHZ_FB_RGBX8888;
    mutex_init(&g.lock); sem_init(&g.browser_sem, 0); g.focus = 3; g.running = 1; g.dirty = 1;
    g.desktop_thread = thread_current();
    g.browser.x = 24; g.browser.y = 89; g.browser.w = (int)g.width * 58 / 100; g.browser.h = (int)g.height - 153;
    right_x = g.browser.x + g.browser.w + 20; usable = (int)g.height - 173;
    g.video_rect = (rect_t){ right_x, 89, (int)g.width - right_x - 24, usable / 2 };
    g.music_rect = (rect_t){ right_x, 109 + usable / 2, (int)g.width - right_x - 24, usable - usable / 2 };
    bytes = read_fixture("C:\\MEDIA\\DEMO.KV64", &n);
    if (!bytes || kz_video_parse(&g.video, bytes, (size_t)n)) { g.failed = 1; kprintf("SHZGUI video fixture rejected\n"); }
    bytes = read_fixture("C:\\MEDIA\\DEMO.KM64", &n);
    if (!bytes || kz_music_parse(&g.music, bytes, (size_t)n)) { g.failed = 1; kprintf("SHZGUI music fixture rejected\n"); }
    /* The initial local page needs no browser-worker round trip. Draw before
     * creating workers or waiting on keyboard/controller state. */
    bytes = read_fixture("C:\\WWW\\INDEX.HTM", &n);
    if (!bytes || kz_html_parse(&g.page, bytes, (size_t)n)) {
        g.failed = 1; strcopy(g.message, "Initial HTML document is missing or invalid.", sizeof g.message);
    } else {
        strcopy(g.url, "C:\\WWW\\INDEX.HTM", sizeof g.url);
        strcopy(g.message, "F8 runs the full native acceptance suite. F6 edits a URL.", sizeof g.message);
    }
    g.video_epoch = ticks_now(); g.video_playing = !!g.video.pixels;
    k64_boot_console_enable(0);
    render();
    g.video_thread = thread_create("gui.video", video_worker, 0);
    g.music_thread = thread_create("gui.music", music_worker, 0);
    g.browser_thread = thread_create("gui.browser", browser_worker, 0);
    if (!g.video_thread || !g.music_thread || !g.browser_thread) { g.failed = 1; return -1; }
    {
        uint64_t parent = kurazy_tree_register_kernel(g.desktop_thread, 0);
        kurazy_tree_register_kernel(g.video_thread, parent);
        kurazy_tree_register_kernel(g.music_thread, parent);
        kurazy_tree_register_kernel(g.browser_thread, parent);
    }
    g.ready = 1;
    if (keyboard_init()) {
        strcopy(g.message, "PS/2 keyboard unavailable: enable firmware USB legacy keyboard emulation.", sizeof g.message);
        kprintf("SHZGUI KEYBOARD unavailable; desktop remains visible\n");
    } else kprintf("SHZGUI KEYBOARD ready backend=i8042-polled\n");
    render();
    kprintf("SHZGUI READY backend=GOP size=%ux%u bpp=32 native_threads=%u,%u,%u\n", g.width, g.height,
            g.video_thread->id, g.music_thread->id, g.browser_thread->id);
    return g.failed ? -1 : 0;
}
int shizukugui_selftest(void)
{
    uint32_t first, second, full; unsigned notes_before; uint8_t invalid[24] = { 'K', 'V', '6', '4' };
    kz_video_t rejected; char path[160]; int failed = 0;
    if (!g.ready) return -1;
    mutex_lock(&g.lock); g.music_playing = 0; g.history_count = 0; mutex_unlock(&g.lock);
    thread_sleep_ms(15);
    if (browser_wait(browser_request("C:\\WWW\\INDEX.HTM", 0))) ++failed;
    if (shizukugui_open(SHIZUKUGUI_VIDEO) || g.focus != SHIZUKUGUI_VIDEO ||
        shizukugui_open(SHIZUKUGUI_MUSIC) || g.focus != SHIZUKUGUI_MUSIC ||
        shizukugui_open(SHIZUKUGUI_BROWSER) || g.focus != SHIZUKUGUI_BROWSER) ++failed;
    mutex_lock(&g.lock); g.video_playing = 0; g.frame = 0; mutex_unlock(&g.lock);
    render(); first = checksum(video_pixels());
    mutex_lock(&g.lock); g.frame = g.video.frames > 1 ? 1 : 0; mutex_unlock(&g.lock);
    render(); second = checksum(video_pixels());
    if (!first || first == second || !kz_video_parse(&rejected, invalid, sizeof invalid)) ++failed;
    if (!g.page.link_count || g.page.line_count < 3) ++failed;
    if (kz_html_resolve("C:\\WWW\\INDEX.HTM", "../secret", path) != -1 ||
        kz_html_resolve("C:\\WWW\\INDEX.HTM", "https://example.org", path) != -2) ++failed;
    key(0x3b); key(0x1c); /* follow the first actual document link through the normal input path */
    if (browser_wait(g.request_id) || !strcmp(g.url, "C:\\WWW\\INDEX.HTM")) ++failed;
    kprintf("SHZGUI BROWSER navigation=%s title=%s\n", g.url, g.page.title);
    key(0x0e); if (browser_wait(g.request_id) || strcmp(g.url, "C:\\WWW\\INDEX.HTM")) ++failed;
    {
        sock_t *listener = 0; thread_t *server = 0; int result = -1;
        if (!net_ensure_init()) {
            net_lock(); listener = sock_new(SK_STREAM, 1);
            if (listener) {
                listener->lip = IP4(127, 0, 0, 1); listener->lport = 18064; listener->bound = 1;
                if (tcp_listen(listener, 1)) { sock_close_kernel(listener); listener = 0; }
            }
            net_unlock();
        }
        if (listener) server = thread_create("gui.http-test", http_test_server, listener);
        if (server) {
            int64_t server_result;
            kurazy_tree_register_kernel(server, kurazy_tree_kernel_id(g.browser_thread));
            result = browser_wait(browser_request("http://127.0.0.1:18064/INDEX.HTM", 0));
            server_result = thread_join(server);
            if (strcmp(g.page.title, "HTTP loopback / real TCP") || server_result) result = -1;
        } else if (listener) { net_lock(); sock_close_kernel(listener); net_unlock(); }
        if (result) ++failed;
        kprintf("SHZGUI HTTP %s loopback=127.0.0.1 port=18064 transport=TCP\n", result ? "FAIL" : "PASS");
        if (browser_wait(browser_request("C:\\WWW\\INDEX.HTM", 0))) ++failed;
    }
    mutex_lock(&g.lock); notes_before = g.notes_played; mutex_unlock(&g.lock);
    key(0x3d); key(0x39); thread_sleep_ms(420); key(0x39); thread_sleep_ms(15);
    if (g.notes_played <= notes_before || (k_inb(0x61) & 3)) ++failed;
    if (!thread_cycles_now(g.video_thread) || !thread_cycles_now(g.music_thread) || !thread_cycles_now(g.browser_thread)) ++failed;
    key(0x3e); render(); key(0x3f); render(); key(0x3b);
    mutex_lock(&g.lock); g.video_playing = 1; g.video_epoch = ticks_now(); g.frame = 0;
    g.music_playing = 0; g.note = 0; g.music_elapsed = 0; g.hz = 0; g.focus = 3; mutex_unlock(&g.lock);
    render(); full = checksum((rect_t){ 0, 0, (int)g.width, (int)g.height });
    if (front_checksum() != full) ++failed;
    g.failed += failed;
    kprintf("SHZGUI SELFTEST %s rendered_checksum=%08x video_checksum=%08x,%08x browser_navigation=2 speaker_notes=%u workers=3 failures=%u\n",
            failed ? "FAIL" : "PASS", full, first, second, g.notes_played - notes_before, failed);
    return failed ? -1 : 0;
}
void shizukugui_run(void)
{
    uint64_t last = 0;
    if (!g.ready) return;
    kprintf("SHZGUI INTERACTIVE ready; F1 browser F2 video F3 music F4 tree F5 utilities F8 acceptance F10 shutdown\n");
    while (!g.quit) {
        uint64_t now = ticks_now(); poll_keyboard();
        if ((g.dirty && now - last >= 50) || (g.focus == 4 && now - last >= 500)) { render(); last = now; }
        thread_sleep_ms(5);
    }
    g.running = 0; sem_post(&g.browser_sem);
    thread_join(g.video_thread); thread_join(g.music_thread); thread_join(g.browser_thread);
    speaker(0); kprintf("SHZGUI SHUTDOWN requested\n");
}
