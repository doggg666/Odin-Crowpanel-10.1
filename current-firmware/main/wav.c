#include "wav.h"
#include <string.h>

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool wav_parse(const uint8_t *buf, size_t len, wav_view_t *view)
{
    if (!buf || !view || len < 12 || memcmp(buf, "RIFF", 4) ||
        memcmp(buf + 8, "WAVE", 4)) return false;
    uint32_t riff_size = le32(buf + 4);
    if (riff_size < 4 || riff_size > len - 8) return false;
    size_t end = (size_t)riff_size + 8;
    bool have_fmt = false, have_data = false;
    uint16_t bits = 0, align = 0;
    size_t data_len = 0;
    wav_view_t v = {0};
    for (size_t off = 12; off < end;) {
        if (end - off < 8) return false;
        const uint8_t *chunk = buf + off;
        size_t size = le32(chunk + 4);
        off += 8;
        if (size > end - off) return false;
        const uint8_t *p = buf + off;
        if (!memcmp(chunk, "fmt ", 4)) {
            if (have_fmt || size < 16) return false;
            v.format = le16(p);
            v.channels = le16(p + 2);
            v.rate = le32(p + 4);
            align = le16(p + 12);
            bits = le16(p + 14);
            if ((v.channels != 1 && v.channels != 2) ||
                v.rate < 8000 || v.rate > 192000 ||
                !((v.format == 1 && bits == 16) || (v.format == 3 && bits == 32)) ||
                align != v.channels * (bits / 8)) return false;
            have_fmt = true;
        } else if (!memcmp(chunk, "data", 4)) {
            if (have_data) return false;
            v.data = p;
            data_len = size;
            have_data = true;
        }
        off += size;
        if (size & 1) {
            if (off == end) return false;
            off++;
        }
    }
    if (!have_fmt || !have_data || !data_len || data_len % align) return false;
    v.frames = data_len / align;
    *view = v;
    return true;
}
