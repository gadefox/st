/* See LICENSE for license details. */

#ifndef _STERM_H_
#define _STERM_H_

#include <stdint.h>
#include <wchar.h>

#include <sys/types.h>

#include "def.h"


/* macros */
#define MIN(a, b)           ((a) < (b) ? (a) : (b))
#define MAX(a, b)           ((a) < (b) ? (b) : (a))
#define LEN(a)	            (sizeof (a) / sizeof (*(a)))
#define BETWEEN(x, a, b)    ((a) <= (x) && (x) <= (b))
#define DIVCEIL(n, d)	    (((n) + (d) - 1) / (d))
#define DEFAULT(a, b)       (a) = (a) ? (a) : (b)
#define LIMIT(x, a, b)      (x) = (x) < (a) ? (a) : (x) > (b) ? (b) : (x)
#define TIMEDIFF(t1, t2)	((t1.tv_sec - t2.tv_sec) * 1000 + (t1.tv_nsec - t2.tv_nsec) / 1E6)
#define MODBIT(x, set, bit)	((set) ? ((x) |= (bit)) : ((x) &= ~(bit)))

#define TRUECOLOR(r,g,b)	(1 << 24 | (r) << 16 | (g) << 8 | (b))
#define IS_TRUECOL(x)		(1 << 24 & (x))

#define tty_read_pending()  (tflags & TReadPending)
#define tty_sync_update()   (tflags & TSyncUpdate)


typedef enum {
    /* term mode: reset in treset */
    MODE_ALTSCREEN  = 1 << 0,
    MODE_WRAP       = 1 << 1,
    MODE_INSERT     = 1 << 2,
    MODE_CRLF       = 1 << 3,
    MODE_ECHO       = 1 << 4,
    MODE_PRINT      = 1 << 5,
    MODE_UTF8       = 1 << 6,

    /* escape state: reset in tputc and csi_handle */
    ESC_START       = 1 << 7,
    ESC_CSI         = 1 << 8,
    ESC_STR         = 1 << 9,   /* DCS, OSC, PM, APC */
    ESC_ALTCHARSET  = 1 << 10,
    ESC_STR_END     = 1 << 11,  /* a final string was encountered */
    ESC_TEST        = 1 << 12,  /* Enter in test mode */
    ESC_UTF8        = 1 << 13,

    /* cursor state: reset in treset */
    CURSOR_WRAPNEXT = 1 << 14,
    CURSOR_ORIGIN   = 1 << 15,

    /* selection: reset in sel_clear */
      /* type */
    SEL_REGULAR     = 0,
	SEL_RECT        = 1 << 16,
      /* snap */
    SNAP_NO         = 0,
	SNAP_WORD       = 1 << 17,
    SNAP_LINE       = 1 << 18,
      /* other */
    SEL_ALTSCREEN   = 1 << 19,

    /* CSI esc seq: set/reset in csi_parse */
    CSI_PRIV        = 1 << 20
} TermFlags;

typedef enum {
	ATTR_NULL       = 0,
	ATTR_BOLD       = 1 << 0,
	ATTR_FAINT      = 1 << 1,
	ATTR_ITALIC     = 1 << 2,
	ATTR_UNDERLINE  = 1 << 3,
	ATTR_BLINK      = 1 << 4,
	ATTR_REVERSE    = 1 << 5,
	ATTR_INVISIBLE  = 1 << 6,
	ATTR_STRUCK     = 1 << 7,
	ATTR_WRAP       = 1 << 8,
	ATTR_WIDE       = 1 << 9,
	ATTR_WDUMMY     = 1 << 10,
	ATTR_BOLD_FAINT = ATTR_BOLD | ATTR_FAINT,
} GlyphAttribute;

typedef uint_least32_t Rune;

typedef struct {
	Rune rune;    /* character code */
	ushort attr;  /* attribute flags */
	ushort fg;    /* foreground color; cache index */
    ushort bg;    /* background color; cache index */
} TermGlyph;

typedef TermGlyph *Line;

typedef union {
	int i;
	uint ui;
	float f;
	const void *v;
	const char *s;
} Arg;


void x_exit (void);
void die (void);
void send_break (const Arg *);

/* tty */
void tty_hangup (void);
int tty_new (const char **argv, uint argn);
uint tty_read (void);
void tty_resize (int, int);
void tty_write (const char *, uint, int);

/* terminal */
void t_draw (int fulldirt);
int t_attr_set (GlyphAttribute);
void t_new (uint, uint);
void t_resize (uint, uint);
void t_attr_set_dirt (int);

/* print */
void print_screen (const Arg *);
void print_toggle (const Arg *);
void print_sel (const Arg *);

/* selection */
void print_sel (const Arg *);
void sel_clear (void);
void sel_start (uint, uint, uint);
void sel_extend (uint, uint, uint, int);
char *sel_get (void);
int t_selected (uint, uint);
int tline_sel_get_margin (uint row, uint *col1, uint *col2);

/* utf8 */
size_t utf8_encode (Rune, char *);

/* memory, init and destroy */
void *x_malloc (uint);
void *x_realloc (void *, uint);

void t_init (void);
void t_free (void);

int tsu_clock (void);


/* config.h globals */
extern char *vtiden;
extern wchar_t *worddelimiters;
extern int allowaltscreen;
extern char *termname;


#endif  /* _STERM_H_ */
