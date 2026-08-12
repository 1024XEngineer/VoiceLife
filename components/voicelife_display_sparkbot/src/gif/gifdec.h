/* Vendored from xiaozhi-esp32@37d1aee main/display/lvgl_display/gif/gifdec.h.
 * 上游：https://github.com/lecram/gifdec（MIT 许可）。仅按上游维护，不得视为
 * VoiceLife 原创代码；许可与改动记录见 components/voicelife_display_sparkbot/README.md。 */
#ifndef GIFDEC_H
#define GIFDEC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <lvgl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct _gd_Palette {
    int size;
    uint8_t colors[0x100 * 3];
} gd_Palette;

typedef struct _gd_GCE {
    uint16_t delay;
    uint8_t tindex;
    uint8_t disposal;
    int input;
    int transparency;
} gd_GCE;

typedef struct _gd_GIF {
    lv_fs_file_t fd;
    const char* data;
    uint8_t is_file;
    uint32_t f_rw_p;
    /* Zero preserves the legacy, unbounded in-memory data API. */
    size_t data_size;
    /* True after an in-memory read or seek exceeds data_size. */
    bool io_error;
    int32_t anim_start;
    uint16_t width, height;
    uint16_t depth;
    int32_t loop_count;
    gd_GCE gce;
    gd_Palette* palette;
    gd_Palette lct, gct;
    void (*plain_text)(struct _gd_GIF* gif, uint16_t tx, uint16_t ty, uint16_t tw, uint16_t th, uint8_t cw, uint8_t ch,
                       uint8_t fg, uint8_t bg);
    void (*comment)(struct _gd_GIF* gif);
    void (*application)(struct _gd_GIF* gif, char id[8], char auth[3]);
    uint16_t fx, fy, fw, fh;
    uint8_t bgindex;
    uint8_t *canvas, *frame;
#if LV_GIF_CACHE_DECODE_DATA
    uint8_t* lzw_cache;
#endif
} gd_GIF;

gd_GIF* gd_open_gif_file(const char* fname);

gd_GIF* gd_open_gif_data(const void* data);

/* Opens one bounded GIF asset without reading into adjacent packed data. */
gd_GIF* gd_open_gif_data_sized(const void* data, size_t length);

void gd_render_frame(gd_GIF* gif, uint8_t* buffer);

int gd_get_frame(gd_GIF* gif);
void gd_rewind(gd_GIF* gif);
void gd_close_gif(gd_GIF* gif);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GIFDEC_H */
