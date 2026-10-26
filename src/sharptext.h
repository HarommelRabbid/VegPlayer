/* sharptext - crisp PGF/PVF text for vita2d at any scale.
 *
 * vita2d's vita2d_pgf/pvf_draw_text render every glyph once, at the font's base size, and then scale the
 * quads (point-sampled, with pen positions truncated to whole pixels). Below 1.0 that drops and doubles pixels
 * (crusty), above 1.0 it magnifies a small bitmap (soft). sharptext instead makes a glyph image for every size it
 * is drawn at and draws it 1:1 on whole pixels:
 *   - PVF (vector outlines): the glyph is rasterised by scePvf at the exact size, so text is sharp at any scale.
 *   - PGF (bitmaps at one size): the glyph is resampled once per size with a proper filter (area average when
 *     shrinking, Catmull-Rom when enlarging), then cached. PGF has no outlines, so large sizes stay a bit softer
 *     than PVF, but there is no shimmer and no uneven spacing.
 * The API mirrors vita2d's (same arguments, y = baseline, scale 1.0 = the size vita2d draws at 1.0), so it is a
 * drop-in replacement. Glyphs are cached per size in 512x512 8-bit atlas pages. Thread-safe.
 */
#ifndef SHARPTEXT_H
#define SHARPTEXT_H

#include <vita2d.h>

typedef struct stext_font stext_font;

stext_font *stext_load_system_pvf(int numFonts, const vita2d_system_pvf_config *configs);
stext_font *stext_load_default_pvf(void);
stext_font *stext_load_custom_pvf(const char *path);
stext_font *stext_load_system_pgf(int numFonts, const vita2d_system_pgf_config *configs);
stext_font *stext_load_default_pgf(void);
stext_font *stext_load_custom_pgf(const char *path);
void stext_free(stext_font *font);

/* returns the width drawn, in pixels */
int stext_draw_text(stext_font *font, int x, int y, unsigned int color, float scale, const char *text);
int stext_draw_textf(stext_font *font, int x, int y, unsigned int color, float scale, const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));
int stext_draw_text_ls(stext_font *font, int x, int y, float linespace, unsigned int color, float scale, const char *text);
void stext_text_dimensions(stext_font *font, float scale, const char *text, int *width, int *height);
int stext_text_width(stext_font *font, float scale, const char *text);
int stext_text_height(stext_font *font, float scale, const char *text);

/* drop every cached glyph (e.g. after showing a lot of one-off sizes); call between frames */
void stext_flush(stext_font *font);

#endif
