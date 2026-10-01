/* SPDX-License-Identifier: GPL-2.0-only */
#include "kurazy_media.h"

static unsigned le16(const uint8_t *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static int equal4(const uint8_t *p, const char *m)
{ return p[0] == (uint8_t)m[0] && p[1] == (uint8_t)m[1] && p[2] == (uint8_t)m[2] && p[3] == (uint8_t)m[3]; }
static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static int space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static void zero(void *p, size_t n) { uint8_t *s = p; while (n--) *s++ = 0; }
static void copy(char *dst, const char *src, unsigned cap)
{ unsigned i = 0; if (!cap) return; while (i + 1 < cap && src[i]) { dst[i] = src[i]; ++i; } dst[i] = 0; }
static int same(const char *a, const char *b)
{ while (*a && *b && lower(*a) == lower(*b)) { ++a; ++b; } return !*a && !*b; }
static int prefix(const char *a, const char *b)
{ while (*b) { if (lower(*a++) != lower(*b++)) return 0; } return 1; }

int kz_video_parse(kz_video_t *v, const uint8_t *b, size_t n)
{
    size_t frame, payload;
    zero(v, sizeof *v);
    if (!b || n < 24 || !equal4(b, "KV64") || le16(b + 4) != 1 || le16(b + 6) != 24) return -1;
    v->width = (uint16_t)le16(b + 8); v->height = (uint16_t)le16(b + 10);
    v->fps = (uint16_t)le16(b + 12); v->frames = (uint16_t)le16(b + 14);
    if (!v->width || !v->height || v->width > 512 || v->height > 512 || !v->fps || v->fps > 60 ||
        !v->frames || v->frames > 600 || b[16] != 24) return -1;
    frame = (size_t)v->width * v->height * 3;
    payload = frame * v->frames;
    if (payload > 32u * 1024u * 1024u || n != 24 + payload) return -1;
    v->pixels = b + 24; v->frame_bytes = frame;
    return 0;
}
uint32_t kz_video_pixel(const kz_video_t *v, unsigned frame, unsigned x, unsigned y)
{
    const uint8_t *p;
    if (!v->pixels || frame >= v->frames || x >= v->width || y >= v->height) return 0;
    p = v->pixels + frame * v->frame_bytes + ((size_t)y * v->width + x) * 3;
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}
int kz_music_parse(kz_music_t *m, const uint8_t *b, size_t n)
{
    unsigned i;
    zero(m, sizeof *m);
    if (!b || n < 16 || !equal4(b, "KM64") || le16(b + 4) != 1 || le16(b + 6) != 16) return -1;
    m->count = (uint16_t)le16(b + 8); m->repeats = (uint16_t)le16(b + 10);
    if (!m->count || m->count > 1024 || !m->repeats || m->repeats > 100 || n != 16u + m->count * 4u) return -1;
    for (i = 0; i < m->count; ++i) {
        unsigned hz = le16(b + 16 + i * 4), ms = le16(b + 18 + i * 4);
        if ((hz && (hz < 20 || hz > 20000)) || !ms || ms > 10000) return -1;
        m->duration_ms += ms;
    }
    m->notes = b + 16;
    return 0;
}
int kz_music_note(const kz_music_t *m, unsigned index, unsigned *hz, unsigned *ms)
{
    if (!m->notes || index >= m->count || !hz || !ms) return -1;
    *hz = le16(m->notes + index * 4); *ms = le16(m->notes + index * 4 + 2); return 0;
}

static void newline(kz_html_t *p, unsigned *col)
{
    if (*col && p->line_count + 1 < KZ_HTML_LINES) { ++p->line_count; *col = 0; }
    else if (*col) p->truncated = 1;
}
static void put(kz_html_t *p, unsigned *col, char c)
{
    if (p->truncated) return;
    if (space(c)) { c = ' '; if (!*col || p->lines[p->line_count][*col - 1] == ' ') return; }
    if ((unsigned char)c < 32 || (unsigned char)c >= 127) c = '?';
    if (*col >= KZ_HTML_COLUMNS) newline(p, col);
    if (p->truncated) return;
    p->lines[p->line_count][(*col)++] = c;
    p->lines[p->line_count][*col] = 0;
}
static char entity(const uint8_t *b, size_t n, size_t *pos)
{
    size_t i = *pos + 1; char s[10]; unsigned j = 0;
    while (i < n && j + 1 < sizeof s && b[i] != ';' && b[i] != '<' && !space((char)b[i])) s[j++] = (char)b[i++];
    s[j] = 0;
    if (i >= n || b[i] != ';') return '&';
    *pos = i;
    if (same(s, "amp")) return '&';
    if (same(s, "lt")) return '<';
    if (same(s, "gt")) return '>';
    if (same(s, "quot")) return '"';
    if (same(s, "apos")) return '\'';
    if (same(s, "nbsp")) return ' ';
    return '?';
}
static void attribute(const char *s, const char *wanted, char *out, unsigned cap)
{
    unsigned p = 0;
    out[0] = 0;
    while (s[p]) {
        char key[24], value[160], quote = 0; unsigned k = 0, v = 0;
        while (space(s[p])) ++p;
        while (s[p] && !space(s[p]) && s[p] != '=') { if (k + 1 < sizeof key) key[k++] = lower(s[p]); ++p; }
        key[k] = 0;
        while (space(s[p])) ++p;
        if (s[p] != '=') { if (s[p]) ++p; continue; }
        ++p; while (space(s[p])) ++p;
        if (s[p] == '\'' || s[p] == '"') quote = s[p++];
        while (s[p] && (quote ? s[p] != quote : !space(s[p]))) {
            if (v + 1 < sizeof value) value[v++] = s[p];
            ++p;
        }
        value[v] = 0; if (quote && s[p]) ++p;
        if (same(key, wanted)) { copy(out, value, cap); return; }
    }
}
int kz_html_parse(kz_html_t *p, const uint8_t *b, size_t n)
{
    size_t i; unsigned col = 0, title_len = 0, label_len = 0; int title = 0, hidden = 0, anchor = -1;
    if (!p || !b || !n || n > 65536) return -1;
    zero(p, sizeof *p); copy(p->title, "Shizuku Browser64", sizeof p->title);
    for (i = 0; i < n; ++i) {
        char c = (char)b[i];
        if (c == '<') {
            char tag[16], attrs[256]; unsigned t = 0, a = 0; int closing;
            if (i + 1 < n && b[i + 1] == '!') { while (i < n && b[i] != '>') ++i; continue; }
            ++i; closing = i < n && b[i] == '/'; if (closing) ++i;
            while (i < n && !space((char)b[i]) && b[i] != '>') { if (t + 1 < sizeof tag) tag[t++] = lower((char)b[i]); ++i; }
            tag[t] = 0;
            while (i < n && b[i] != '>') { if (a + 1 < sizeof attrs) attrs[a++] = (char)b[i]; ++i; }
            attrs[a] = 0;
            if (same(tag, "script") || same(tag, "style")) { hidden = !closing; continue; }
            if (same(tag, "title")) { title = !closing; if (title) { title_len = 0; p->title[0] = 0; } continue; }
            if (same(tag, "a")) {
                if (closing) anchor = -1;
                else if (p->link_count < KZ_HTML_LINKS) {
                    char href[160]; attribute(attrs, "href", href, sizeof href);
                    if (href[0]) {
                        anchor = (int)p->link_count++; label_len = 0;
                        copy(p->links[anchor].href, href, sizeof p->links[anchor].href);
                        p->links[anchor].line = p->line_count;
                    }
                }
                continue;
            }
            if (same(tag, "p") || same(tag, "br") || same(tag, "h1") || same(tag, "h2") || same(tag, "li") ||
                same(tag, "div") || same(tag, "tr") || same(tag, "hr")) {
                newline(p, &col); if (same(tag, "li") && !closing) { put(p, &col, '*'); put(p, &col, ' '); }
            }
            continue;
        }
        if (hidden) continue;
        if (c == '&') c = entity(b, n, &i);
        if (title) { if (title_len + 1 < sizeof p->title) { p->title[title_len++] = c; p->title[title_len] = 0; } continue; }
        put(p, &col, c);
        if (anchor >= 0 && label_len + 1 < sizeof p->links[anchor].label) {
            if (space(c)) c = ' ';
            p->links[anchor].label[label_len++] = c; p->links[anchor].label[label_len] = 0;
        }
    }
    ++p->line_count;
    return 0;
}
int kz_html_resolve(const char *current, const char *href, char out[160])
{
    unsigned i = 0, j = 0, segment = 0;
    char raw[320];
    if (!current || !href || !*href) return -1;
    if (prefix(href, "http://")) {
        uint32_t ip; unsigned port; char path[160];
        if (kz_http_url(href, &ip, &port, path)) return -1;
        for (i = 0; href[i]; ++i) if (i >= 159) return -1;
        copy(out, href, 160); return 0;
    }
    if (prefix(current, "http://")) {
        uint32_t ip; unsigned port; char path[160];
        if (kz_http_url(current, &ip, &port, path)) return -1;
        if (href[0] == '/') { for (i = 7; current[i] && current[i] != '/'; ++i) {} }
        else { unsigned last = 7; for (i = 7; current[i]; ++i) if (current[i] == '/') last = i + 1; i = last; }
        if (i >= 159) return -1;
        for (j = 0; j < i; ++j) out[j] = current[j];
        for (j = 0; href[j] && href[j] != '#'; ++j) { if (i + 1 >= 160 || href[j] == ':' || href[j] < 32) return -1; out[i++] = href[j]; }
        out[i] = 0; return kz_http_url(out, &ip, &port, path);
    }
    while (href[i] && i < 8) { if (href[i] == ':') return -2; ++i; }
    if (href[0] == '/' || href[0] == '\\') copy(raw, "C:\\WWW\\", sizeof raw);
    else {
        copy(raw, current, sizeof raw);
        for (i = 0; raw[i]; ++i) if (raw[i] == '\\' || raw[i] == '/') j = i + 1;
        raw[j] = 0;
    }
    for (j = 0; raw[j]; ++j) {}
    i = href[0] == '/' || href[0] == '\\' ? 1 : 0;
    while (href[i] && href[i] != '#' && href[i] != '?' && j + 1 < sizeof raw) raw[j++] = href[i++];
    raw[j] = 0;
    if (href[i] && href[i] != '#' && href[i] != '?') return -1;
    if (raw[0] != 'C' || raw[1] != ':' || raw[2] != '\\' ||
        lower(raw[3]) != 'w' || lower(raw[4]) != 'w' || lower(raw[5]) != 'w' || raw[6] != '\\') return -1;
    for (i = 7, j = 7, segment = 7; raw[i]; ++i) {
        char c = raw[i];
        if (c == '/' || c == '\\') {
            unsigned len = j - segment;
            if ((len == 1 && out[segment] == '.') || (len == 2 && out[segment] == '.' && out[segment + 1] == '.')) return -1;
            if (len) { if (j + 1 >= 160) return -1; out[j++] = '\\'; segment = j; }
        } else { if (c == ':' || c < 32 || j + 1 >= 160) return -1; out[j++] = c; }
    }
    if (j == segment || (j - segment == 1 && out[segment] == '.') ||
        (j - segment == 2 && out[segment] == '.' && out[segment + 1] == '.')) return -1;
    for (i = 0; i < 7; ++i) out[i] = "C:\\WWW\\"[i];
    out[j] = 0;
    return 0;
}
int kz_http_url(const char *url, uint32_t *ip, unsigned *port, char path[160])
{
    unsigned pos = 7, component, value, digits, i, p = 80; uint32_t addr = 0;
    if (!url || !ip || !port || !path || !prefix(url, "http://")) return -1;
    for (component = 0; component < 4; ++component) {
        value = digits = 0;
        while (url[pos] >= '0' && url[pos] <= '9') {
            value = value * 10 + (unsigned)(url[pos++] - '0');
            if (++digits > 3 || value > 255) return -1;
        }
        if (!digits) return -1;
        addr = (addr << 8) | value;
        if (component != 3 && url[pos++] != '.') return -1;
    }
    if (url[pos] == ':') {
        ++pos; digits = p = 0;
        while (url[pos] >= '0' && url[pos] <= '9') {
            p = p * 10 + (unsigned)(url[pos++] - '0'); if (++digits > 5 || p > 65535) return -1;
        }
        if (!digits || !p) return -1;
    }
    if (url[pos] && url[pos] != '/') return -1;
    if (!url[pos]) copy(path, "/", 160);
    else {
        for (i = 0; url[pos] && url[pos] != '#'; ++pos) {
            char c = url[pos]; if (i + 1 >= 160 || c < 33 || c >= 127) return -1; path[i++] = c;
        }
        path[i] = 0;
    }
    *ip = addr; *port = p; return 0;
}
int kz_http_body(const uint8_t *r, size_t n, const uint8_t **body, size_t *body_size)
{
    size_t pos = 0, end = 0;
    if (!r || !body || !body_size || n < 16 || n > 65536 ||
        !prefix((const char *)r, "HTTP/1.") || r[8] != ' ' || r[9] != '2' || r[10] != '0' || r[11] != '0') return -1;
    while (pos + 1 < n) {
        char line[512]; unsigned i = 0;
        while (pos < n && r[pos] != '\r' && r[pos] != '\n') {
            if (i + 1 >= sizeof line) return -1;
            line[i++] = (char)r[pos++];
        }
        line[i] = 0;
        if (pos + 1 >= n || r[pos] != '\r' || r[pos + 1] != '\n') return -1;
        pos += 2;
        if (!i) { end = pos; break; }
        if (prefix(line, "Transfer-Encoding:") || prefix(line, "Content-Encoding:")) return -1;
    }
    if (!end || end >= n) return -1;
    *body = r + end; *body_size = n - end; return 0;
}
