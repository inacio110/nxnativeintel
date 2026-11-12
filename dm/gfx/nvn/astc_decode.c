#include "astc_decode.h"

#include "astcenc.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static const unsigned astc_blocks[14][2] = {
    {4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6},
    {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}
};

static struct astcenc_context *contexts[2][14];
static pthread_mutex_t context_lock = PTHREAD_MUTEX_INITIALIZER;
static int size_mul(size_t a, size_t b, size_t *result) {
    if (a && b > SIZE_MAX / a) return 0;
    *result = a * b;
    return 1;
}

int dm_astc_decode_rgba8(int format, unsigned block_width, unsigned block_height,
                         unsigned width, unsigned height, unsigned depth,
                         const uint8_t *src, size_t src_bytes,
                         uint8_t *dst, size_t row_bytes, size_t image_bytes) {
    if (format < 0x79 || format > 0x94 || !width || !height || !depth || !src || !src_bytes || !dst)
        return 0;
    size_t minimum_row, minimum_image;
    if (!size_mul((size_t)width, 4, &minimum_row) || !row_bytes || row_bytes < minimum_row ||
        !size_mul(row_bytes, (size_t)height, &minimum_image) || image_bytes < minimum_image)
        return 0;

    unsigned block = (unsigned)(format - 0x79) % 14;
    if (astc_blocks[block][0] != block_width || astc_blocks[block][1] != block_height)
        return 0;
    unsigned srgb = format >= 0x87;

    size_t plane_pixels, plane_bytes, decoded_bytes, planes_bytes;
    if (!size_mul((size_t)width, (size_t)height, &plane_pixels) || !size_mul(plane_pixels, 4, &plane_bytes) ||
        !size_mul(plane_bytes, (size_t)depth, &decoded_bytes) || !size_mul((size_t)depth, sizeof(void *), &planes_bytes))
        return 0;
    uint8_t *decoded = malloc(decoded_bytes);
    void **planes = malloc(planes_bytes);
    if (!decoded || !planes) { free(decoded); free(planes); return 0; }
    for (unsigned z = 0; z < depth; z++) planes[z] = decoded + (size_t)z * plane_bytes;

    pthread_mutex_lock(&context_lock);
    struct astcenc_context *ctx = contexts[srgb][block];
    if (!ctx) {
        struct astcenc_config config;
        enum astcenc_profile profile = srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR;
        enum astcenc_error e = astcenc_config_init(profile, block_width, block_height, 1,
                                                     ASTCENC_PRE_FASTEST, ASTCENC_FLG_DECOMPRESS_ONLY,
                                                     &config);
        if (e == ASTCENC_SUCCESS)
            e = astcenc_context_alloc(&config, 1, &ctx, NULL);
        if (e != ASTCENC_SUCCESS) ctx = NULL;
        contexts[srgb][block] = ctx;
    }

    enum astcenc_error status = ASTCENC_ERR_BAD_CONTEXT;
    if (ctx) {
        struct astcenc_image image = {width, height, depth, ASTCENC_TYPE_U8, planes};
        const struct astcenc_swizzle swizzle = {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
        status = astcenc_decompress_image(ctx, src, src_bytes, &image, &swizzle, 0);
        astcenc_decompress_reset(ctx);
    }
    pthread_mutex_unlock(&context_lock);

    if (status == ASTCENC_SUCCESS) {
        for (unsigned z = 0; z < depth; z++)
            for (unsigned y = 0; y < height; y++)
                memcpy(dst + (size_t)z * image_bytes + (size_t)y * row_bytes,
                       decoded + (size_t)z * plane_bytes + (size_t)y * width * 4,
                       (size_t)width * 4);
    }
    free(planes);
    free(decoded);
    return status == ASTCENC_SUCCESS;
}
