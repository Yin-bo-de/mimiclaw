#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DISPLAY_ZH12_WIDTH 12
#define DISPLAY_ZH12_HEIGHT 12
#define DISPLAY_ZH12_BITMAP_BYTES 24

typedef struct {
    uint32_t codepoint;
    uint8_t bitmap[DISPLAY_ZH12_BITMAP_BYTES];
} display_zh12_glyph_t;

const display_zh12_glyph_t *display_font_zh12_find(uint32_t codepoint);

#ifdef __cplusplus
}
#endif
