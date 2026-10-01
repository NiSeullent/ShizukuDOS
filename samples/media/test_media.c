/* SPDX-License-Identifier: GPL-2.0-only */
#include "../../shizukudos/kernel64/kurazy_media.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *load(const char *root, const char *leaf, size_t *n)
{
    char path[1024]; FILE *f; long size; unsigned char *data;
    assert(snprintf(path, sizeof path, "%s/%s", root, leaf) > 0);
    f = fopen(path, "rb"); assert(f);
    assert(!fseek(f, 0, SEEK_END)); size = ftell(f); assert(size > 0);
    rewind(f); data = malloc((size_t)size); assert(data);
    assert(fread(data, 1, (size_t)size, f) == (size_t)size); fclose(f); *n = (size_t)size; return data;
}
int main(int argc, char **argv)
{
    kz_video_t video; kz_music_t music; kz_html_t page;
    unsigned char *data; size_t n; unsigned hz, ms, i, port; uint32_t ip; char path[160];
    assert(argc == 2);
    data = load(argv[1], "DEMO.KV64", &n);
    assert(!kz_video_parse(&video, data, n)); assert(video.width == 160 && video.frames == 24);
    assert(kz_video_pixel(&video, 0, 0, 0) != kz_video_pixel(&video, 1, 0, 0));
    for (i = 0; i < 24; ++i) assert(kz_video_parse(&video, data, i));
    assert(kz_video_parse(&video, data, n - 1));
    data[8] = 0; data[9] = 0; assert(kz_video_parse(&video, data, n)); free(data);
    data = load(argv[1], "DEMO.KM64", &n); assert(!kz_music_parse(&music, data, n));
    assert(!kz_music_note(&music, 0, &hz, &ms) && hz == 440 && ms == 180);
    assert(kz_music_note(&music, music.count, &hz, &ms));
    assert(kz_music_parse(&music, data, n - 1));
    data[18] = 0; data[19] = 0; assert(kz_music_parse(&music, data, n)); free(data);
    data = load(argv[1], "WWW/INDEX.HTM", &n); assert(!kz_html_parse(&page, data, n));
    assert(page.link_count == 3 && strstr(page.title, "ShizukuDOS"));
    assert(!strcmp(page.links[0].href, "KURAZY.HTM"));
    assert(!kz_html_resolve("C:\\WWW\\INDEX.HTM", page.links[0].href, path));
    assert(!strcmp(path, "C:\\WWW\\KURAZY.HTM"));
    assert(!kz_html_resolve("C:\\WWW\\INDEX.HTM", "/MODES.HTM", path));
    assert(kz_html_resolve("C:\\WWW\\INDEX.HTM", "../secret", path) == -1);
    assert(kz_html_resolve("C:\\WWW\\INDEX.HTM", "https://example.org", path) == -2);
    assert(!kz_http_url("http://127.0.0.1:18064/INDEX.HTM", &ip, &port, path));
    assert(ip == 0x7f000001 && port == 18064 && !strcmp(path, "/INDEX.HTM"));
    assert(kz_http_url("http://999.0.0.1/", &ip, &port, path));
    assert(kz_http_url("http://1.2.3.4:65536/", &ip, &port, path));
    assert(kz_http_url("http://example.org/", &ip, &port, path));
    assert(!kz_html_resolve("http://127.0.0.1:18064/INDEX.HTM", "NEXT.HTM", path));
    assert(!strcmp(path, "http://127.0.0.1:18064/NEXT.HTM"));
    { const char response[] = "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n<h1>actual response</h1>";
      const uint8_t *body; size_t body_size;
      assert(!kz_http_body((const uint8_t *)response, sizeof response - 1, &body, &body_size));
      assert(body_size == strlen("<h1>actual response</h1>") && !memcmp(body, "<h1>", 4));
      assert(kz_http_body((const uint8_t *)response, 30, &body, &body_size)); }
    { const char response[] = "HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\nx";
      const uint8_t *body; size_t body_size;
      assert(kz_http_body((const uint8_t *)response, sizeof response - 1, &body, &body_size)); }
    free(data);
    { const char html[] = "<title>A &amp; B</title><script>SECRET</script><p>Hello &lt;DOS&gt;</p><a href='NEXT.HTM'>next</a>";
      assert(!kz_html_parse(&page, (const uint8_t *)html, sizeof html - 1));
      assert(!strcmp(page.title, "A & B") && page.link_count == 1);
      for (i = 0; i < page.line_count; ++i) assert(!strstr(page.lines[i], "SECRET")); }
    puts("kurazy media: fixtures, malformed data, paths, HTML links/entities, IPv4 HTTP URLs/response bounds: PASS");
    return 0;
}
