/*
 * png_write.c - minimal PNG encoder, no external dependencies.
 *
 * A PNG's IDAT payload is a zlib stream. zlib permits DEFLATE blocks of type 00
 * ("stored"), which are just a 5-byte header followed by literal bytes. So a
 * valid PNG needs no compressor at all - only CRC-32 (for the chunks) and
 * Adler-32 (for the zlib trailer). Files are ~3x larger than a real encoder
 * would produce, which is irrelevant for 800x480 screenshots.
 */

#include "png_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------------- crc32 --- */

static uint32_t s_crc_table[256];
static int      s_crc_table_ready = 0;

static void crc_table_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        s_crc_table[n] = c;
    }
    s_crc_table_ready = 1;
}

/* xor-in / xor-out on every call, so chaining works: c = crc32(crc32(0,a,n),b,m) */
static uint32_t crc32_buf(uint32_t crc, const uint8_t *buf, size_t len)
{
    if (!s_crc_table_ready) crc_table_init();
    crc ^= 0xFFFFFFFFu;
    while (len--) crc = s_crc_table[(crc ^ *buf++) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* --------------------------------------------------------------- adler32 --- */

static uint32_t adler32_buf(const uint8_t *buf, size_t len)
{
    uint32_t a = 1, b = 0;
    while (len) {
        size_t chunk = len > 5552 ? 5552 : len;  /* keeps a,b below overflow */
        len -= chunk;
        while (chunk--) {
            a += *buf++;
            b += a;
        }
        a %= 65521u;
        b %= 65521u;
    }
    return (b << 16) | a;
}

/* ------------------------------------------------------------- primitives -- */

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static int write_chunk(FILE *f, const char type[4], const uint8_t *data, size_t len)
{
    uint8_t hdr[8];
    uint8_t crcbuf[4];
    uint32_t crc;

    put_be32(hdr, (uint32_t)len);
    memcpy(hdr + 4, type, 4);
    if (fwrite(hdr, 1, 8, f) != 8) return -1;
    if (len && fwrite(data, 1, len, f) != len) return -1;

    crc = crc32_buf(0, (const uint8_t *)type, 4);
    if (len) crc = crc32_buf(crc, data, len);
    put_be32(crcbuf, crc);
    if (fwrite(crcbuf, 1, 4, f) != 4) return -1;
    return 0;
}

/* ------------------------------------------------------------------- api --- */

int png_write_rgb(const char *path, const uint8_t *rgb, int w, int h)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    FILE *f = NULL;
    uint8_t ihdr[13];
    uint8_t *raw = NULL;   /* filtered scanlines */
    uint8_t *zs  = NULL;   /* zlib stream */
    size_t raw_len, zs_len, zs_pos, raw_pos;
    uint32_t adler;
    int rc = -1;

    if (!path || !rgb || w <= 0 || h <= 0) return -1;

    /* Each scanline is one filter byte (0 = None) followed by w RGB triplets. */
    raw_len = (size_t)h * ((size_t)w * 3u + 1u);
    raw = (uint8_t *)malloc(raw_len);
    if (!raw) goto done;

    for (int y = 0; y < h; y++) {
        uint8_t *dst = raw + (size_t)y * ((size_t)w * 3u + 1u);
        *dst++ = 0;
        memcpy(dst, rgb + (size_t)y * (size_t)w * 3u, (size_t)w * 3u);
    }

    /*
     * zlib stream = 2 header bytes + stored DEFLATE blocks + 4 Adler bytes.
     * A stored block carries at most 65535 payload bytes and costs 5 bytes of
     * header (BFINAL/BTYPE byte, LEN LE16, NLEN LE16).
     */
    {
        size_t nblocks = (raw_len + 65534u) / 65535u;
        if (nblocks == 0) nblocks = 1;
        zs_len = 2u + nblocks * 5u + raw_len + 4u;
    }
    zs = (uint8_t *)malloc(zs_len);
    if (!zs) goto done;

    zs_pos = 0;
    zs[zs_pos++] = 0x78;  /* CM=8 (deflate), CINFO=7 (32K window) */
    zs[zs_pos++] = 0x01;  /* FCHECK so that 0x7801 % 31 == 0, no preset dict  */

    raw_pos = 0;
    do {
        size_t n = raw_len - raw_pos;
        int final;
        if (n > 65535u) n = 65535u;
        final = (raw_pos + n >= raw_len) ? 1 : 0;

        zs[zs_pos++] = (uint8_t)final;             /* BFINAL, BTYPE = 00 stored */
        zs[zs_pos++] = (uint8_t)(n & 0xFFu);       /* LEN  (little endian)      */
        zs[zs_pos++] = (uint8_t)((n >> 8) & 0xFFu);
        zs[zs_pos++] = (uint8_t)(~n & 0xFFu);      /* NLEN (one's complement)   */
        zs[zs_pos++] = (uint8_t)((~n >> 8) & 0xFFu);
        if (n) memcpy(zs + zs_pos, raw + raw_pos, n);
        zs_pos += n;
        raw_pos += n;
    } while (raw_pos < raw_len);

    adler = adler32_buf(raw, raw_len);
    put_be32(zs + zs_pos, adler);
    zs_pos += 4;

    f = fopen(path, "wb");
    if (!f) goto done;
    if (fwrite(sig, 1, sizeof sig, f) != sizeof sig) goto done;

    put_be32(ihdr + 0, (uint32_t)w);
    put_be32(ihdr + 4, (uint32_t)h);
    ihdr[8]  = 8;  /* bit depth       */
    ihdr[9]  = 2;  /* colour type: RGB truecolour */
    ihdr[10] = 0;  /* compression: deflate */
    ihdr[11] = 0;  /* filter method 0 */
    ihdr[12] = 0;  /* no interlace    */
    if (write_chunk(f, "IHDR", ihdr, sizeof ihdr) != 0) goto done;
    if (write_chunk(f, "IDAT", zs, zs_pos) != 0) goto done;
    if (write_chunk(f, "IEND", NULL, 0) != 0) goto done;

    rc = 0;

done:
    if (f && fclose(f) != 0) rc = -1;
    free(raw);
    free(zs);
    return rc;
}
