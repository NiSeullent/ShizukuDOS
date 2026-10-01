/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef KURAZY_MEDIA_H
#define KURAZY_MEDIA_H
#include <stddef.h>
#include <stdint.h>

/* Deliberately small, original, bounded offline media formats. All integer
 * fields are little endian; parsers never cast an unaligned input header. */
typedef struct {
    const uint8_t *pixels;
    uint16_t width, height, fps, frames;
    size_t frame_bytes;
} kz_video_t;
typedef struct {
    const uint8_t *notes;
    uint16_t count, repeats;
    uint32_t duration_ms;
} kz_music_t;
int kz_video_parse(kz_video_t *v, const uint8_t *bytes, size_t n);
uint32_t kz_video_pixel(const kz_video_t *v, unsigned frame, unsigned x, unsigned y);
int kz_music_parse(kz_music_t *m, const uint8_t *bytes, size_t n);
int kz_music_note(const kz_music_t *m, unsigned index, unsigned *hz, unsigned *ms);

#define KZ_HTML_LINES 48
#define KZ_HTML_COLUMNS 64
#define KZ_HTML_LINKS 12
typedef struct {
    char label[64], href[160];
    unsigned line;
} kz_html_link_t;
typedef struct {
    char title[80];
    char lines[KZ_HTML_LINES][KZ_HTML_COLUMNS + 1];
    kz_html_link_t links[KZ_HTML_LINKS];
    unsigned line_count, link_count;
    int truncated;
} kz_html_t;
int kz_html_parse(kz_html_t *page, const uint8_t *bytes, size_t n);
/* Resolve a packaged file URL to C:\WWW or a supported numeric IPv4 HTTP URL.
 * Unsupported network schemes return -2. */
int kz_html_resolve(const char *current, const char *href, char out[160]);
/* Minimal HTTP/1.0: numeric IPv4 URL and bounded, uncompressed, nonchunked body. */
int kz_http_url(const char *url, uint32_t *ip, unsigned *port, char path[160]);
int kz_http_body(const uint8_t *response, size_t n, const uint8_t **body, size_t *body_size);
#endif
