#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The screen classifier needs brightness only, and the screenshots capssc hands
 * back are baseline JPEGs. Decoding the DC coefficient of every 8x8 block is
 * enough: its value is the block's mean brightness, so the result is a little
 * 1-pixel-per-8-pixels image - exactly the resolution the classifier's regions
 * are measured on - without an IDCT, a colour transform or any allocation. */
#define JPEG_DC_MAX_BLOCKS_X 192
#define JPEG_DC_MAX_BLOCKS_Y 136

typedef struct {
    int blocksW;      /* luma blocks across (8 pixels each) */
    int blocksH;      /* luma blocks down */
    bool complete;    /* every luma block was decoded */
    uint8_t mean[JPEG_DC_MAX_BLOCKS_Y][JPEG_DC_MAX_BLOCKS_X];
} JpegDcImage;

/* Decodes the luma plane's DC coefficients. Returns false for anything that is
 * not a baseline (SOF0), 8-bit JPEG this decoder understands; *image is then
 * left with blocksW/blocksH = 0. */
bool jpegDcDecode(const uint8_t* data, size_t size, JpegDcImage* image);
