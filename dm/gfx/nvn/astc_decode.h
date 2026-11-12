#pragma once

#include <stddef.h>
#include <stdint.h>

// Decode one ASTC image into RGBA8. row_bytes and image_bytes describe the
// padded destination layout used by Metal's buffer-to-texture blit.
int dm_astc_decode_rgba8(int format, unsigned block_width, unsigned block_height,
                         unsigned width, unsigned height, unsigned depth,
                         const uint8_t *src, size_t src_bytes,
                         uint8_t *dst, size_t row_bytes, size_t image_bytes);
