/* See LICENSE for license details. */

#ifndef _XWIN_H_
#define _XWIN_H_

#include "def.h"


typedef enum {
    /* mode */
	MODE_VISIBLE     = 1 << 0,
	MODE_FOCUSED     = 1 << 1,
	MODE_APPKEYPAD   = 1 << 2,
	MODE_MOUSEBTN    = 1 << 3,
	MODE_MOUSEMOTION = 1 << 4,
	MODE_REVERSE     = 1 << 5,
	MODE_KBDLOCK     = 1 << 6,
	MODE_HIDE        = 1 << 7,
	MODE_APPCURSOR   = 1 << 8,
	MODE_MOUSESGR    = 1 << 9,
	MODE_8BIT        = 1 << 10,
	MODE_BLINK       = 1 << 11,
	MODE_FBLINK      = 1 << 12,
	MODE_FOCUS       = 1 << 13,
	MODE_MOUSEX10    = 1 << 14,
	MODE_MOUSEMANY   = 1 << 15,
	MODE_BRCKTPASTE  = 1 << 16,
	MODE_NUMLOCK     = 1 << 17,
	MODE_MOUSE       = MODE_MOUSEBTN | MODE_MOUSEMOTION | MODE_MOUSEX10 | MODE_MOUSEMANY,
    /* fonts */
    FontRegularBadSlant     = 1 << 18,
    FontRegularBadWeight    = 1 << 19,
    FontItalicBadSlant      = 1 << 20,
    FontItalicBadWeight     = 1 << 21,
    FontBoldItalicBadSlant  = 1 << 22,
    FontBoldItalicBadWeight = 1 << 23,
    FontBoldBadSlant        = 1 << 24,
    FontBoldBadWeight       = 1 << 25
} TermWindowFlags;

void x_bell (void);
void x_clip_copy (void);
void x_cursor_draw (Rune rune, GlyphAttribute attr, uint col, uint row);
void x_cursor_remove (TermGlyph *tg, uint col, uint row);
void x_line_draw (Line, uint, uint, uint, uint);
void x_draw_finish (void);

/* color */
void x_colors_load_index (void);
int x_color_load_rgb (uint r, uint g, uint b);
int x_color_load_faint (uint idx);
int x_color_get (uint idx, byte *r, byte *g, byte *b);
int x_color_set_name (uint, const char *);

int x_set_cursor (int);
void x_set_mode (int, uint);
void x_set_pointer_motion (int);
void x_set_sel (char *);
int x_is_mode_visible (void);
void x_im_spot (int, int);
void x_free (void);

#ifdef FEATURE_TITLE
int x_set_title (const char *);
int x_set_icon_title (const char *);
char * x_get_title (void);
char * x_get_icon_title (void);
#endif  /* FEATURE_TITLE */


#endif  /* _XWIN_H_ */
