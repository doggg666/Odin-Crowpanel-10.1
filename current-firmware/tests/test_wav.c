#include "wav.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void put16(uint8_t *p, unsigned v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, unsigned v)
{
    put16(p, v); put16(p + 2, v >> 16);
}

static size_t make_wav(uint8_t *b, unsigned format, unsigned bits, unsigned channels)
{
    unsigned bytes = 4 * channels * bits / 8;
    memset(b, 0, 128);
    memcpy(b, "RIFF", 4); put32(b + 4, 36 + bytes);
    memcpy(b + 8, "WAVEfmt ", 8); put32(b + 16, 16);
    put16(b + 20, format); put16(b + 22, channels); put32(b + 24, 24000);
    put32(b + 28, 24000 * channels * bits / 8);
    put16(b + 32, channels * bits / 8); put16(b + 34, bits);
    memcpy(b + 36, "data", 4); put32(b + 40, bytes);
    return 44 + bytes;
}

int main(void)
{
    uint8_t b[128];
    wav_view_t v;
    for (unsigned ch = 1; ch <= 2; ch++) {
        for (unsigned fmt = 1; fmt <= 3; fmt += 2) {
            size_t n = make_wav(b, fmt, fmt == 1 ? 16 : 32, ch);
            assert(wav_parse(b, n, &v));
            assert(v.frames == 4 && v.channels == ch && v.rate == 24000 && v.format == fmt);
            for (size_t cut = 0; cut < n; cut++) assert(!wav_parse(b, cut, &v));
        }
    }
    const unsigned bad[][3] = {{1,8,1}, {1,24,1}, {6,16,1}, {1,16,0}, {1,16,3}, {3,16,1}};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        size_t n = make_wav(b, bad[i][0], bad[i][1], bad[i][2]);
        assert(!wav_parse(b, n, &v));
    }
    size_t n = make_wav(b, 1, 16, 1);
    b[0] = 'X'; assert(!wav_parse(b, n, &v));
    n = make_wav(b, 1, 16, 1);
    b[8] = 'X'; assert(!wav_parse(b, n, &v));
    n = make_wav(b, 1, 16, 1);
    put32(b + 16, 0xffffffff); assert(!wav_parse(b, n, &v));
    n = make_wav(b, 1, 16, 1);
    put32(b + 40, 0xffffffff); assert(!wav_parse(b, n, &v));
    n = make_wav(b, 1, 16, 1);
    put16(b + 32, 4); assert(!wav_parse(b, n, &v));
    n = make_wav(b, 1, 16, 1);
    put32(b + 24, 0); assert(!wav_parse(b, n, &v));
    /* A complete RIFF container with an incomplete fmt payload. */
    make_wav(b, 1, 16, 1);
    put32(b + 4, 16); assert(!wav_parse(b, 24, &v));
    /* Unknown odd-sized chunks must include their alignment byte. */
    n = make_wav(b, 1, 16, 1);
    memmove(b + 22, b + 12, n - 12);
    memcpy(b + 12, "JUNK", 4); put32(b + 16, 1); b[20] = 0; b[21] = 0;
    put32(b + 4, (unsigned)n + 2);
    assert(wav_parse(b, n + 10, &v));
    puts("WAV parser tests passed");
}
