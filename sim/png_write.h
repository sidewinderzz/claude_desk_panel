/*
 * png_write.h - dependency-free PNG writer.
 *
 * Emits a truecolour 8-bit-per-channel PNG using "stored" (uncompressed)
 * DEFLATE blocks wrapped in a zlib stream. No libpng, no zlib, no miniz.
 */
#ifndef PNG_WRITE_H
#define PNG_WRITE_H

#include <stdint.h>

/* rgb points at w*h*3 bytes, row-major, R,G,B. Returns 0 on success. */
int png_write_rgb(const char *path, const uint8_t *rgb, int w, int h);

#endif /* PNG_WRITE_H */
