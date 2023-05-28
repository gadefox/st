/* See LICENSE for license details. */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include <sys/select.h>
#include <sys/wait.h>

#include "args.h"
#include "win.h"
#include "thunk.h"
#include "strutil.h"
#include "verbose.h"

#if defined(__linux)
 #include <pty.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
 #include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
 #include <libutil.h>
#endif


/* Arbitrary sizes */
#define UTF_INVALID  0xFFFD
#define UTF_SIZ      4
#define ESC_BUF_SIZ  (UTF_SIZ << 7)
#define ESC_ARG_SIZ  16
#define STR_BUF_SIZ  ESC_BUF_SIZ
#define STR_ARG_SIZ  ESC_ARG_SIZ

/* macros */
#define term_flag(f)  (term.flags & (f))


#define ISCONTROLC0(c)   (BETWEEN ((c), 0, 0x1f) || (c) == 0x7f)
#define ISCONTROLC1(c)   (BETWEEN ((c), 0x80, 0x9f))
#define ISCONTROL(c)     (ISCONTROLC0 (c) || ISCONTROLC1 (c))
#define ISDELIM(u)       ((u) != '\0' && wcschr (worddelimiters, (u)))

#define tty_sync_update_end()    { tflags &= ~TSyncUpdate; }
#define tty_sync_update_begin()  { tflags |= TSyncUpdate; }

#define tty_read_pending_end()    { tflags &= ~TReadPending; }
#define tty_read_pending_begin()  { tflags |= TReadPending; }

#define MODE_MASK    (MODE_WRAP | MODE_INSERT | MODE_ALTSCREEN | MODE_CRLF | MODE_ECHO | MODE_PRINT | MODE_UTF8)
#define ESC_MASK     (ESC_START | ESC_CSI | ESC_STR | ESC_ALTCHARSET | ESC_STR_END | ESC_TEST | ESC_UTF8)
#define CURSOR_MASK  (CURSOR_WRAPNEXT | CURSOR_ORIGIN)
#define SEL_MASK     (SEL_RECT | SNAP_WORD | SNAP_LINE | SEL_ALTSCREEN)


static const char esc_type_CSI[]    = "CSI";
static const char esc_type_TEST[]   = "TEST";
static const char esc_type_UTF8[]   = "UTF8";
static const char esc_type_DSC[]    = "DSC";
static const char esc_type_APC[]    = "APC";
static const char esc_type_PM[]     = "PM";
static const char esc_type_OSC[]    = "OSC";
static const char esc_type_LS2[]    = "LS2";
static const char esc_type_LS3[]    = "LS3";
static const char esc_type_GZD4[]   = "GZD4";
static const char esc_type_G1D4[]   = "G1D4";
static const char esc_type_G2D4[]   = "G2D4";
static const char esc_type_G3D4[]   = "D3D4";
static const char esc_type_IND[]    = "IND";
static const char esc_type_NEL[]    = "NEL";
static const char esc_type_HTS[]    = "HTS";
static const char esc_type_RI[]     = "RI";
static const char esc_type_DECID[]  = "DECID";
static const char esc_type_DECPAM[] = "DECPAM";
static const char esc_type_DECPNM[] = "DECPNM";
static const char esc_type_DECSC[]  = "DECSC";
static const char esc_type_DECRC[]  = "DECRC";
static const char esc_type_ST[]     = "ST";

static uchar utfbyte [UTF_SIZ + 1] = {     0x80,    0,  0xC0,   0xE0,  0xF0    };
static uchar utfmask [UTF_SIZ + 1] = {     0xC0, 0x80,  0xE0,   0xF0,  0xF8    };
static Rune  utfmin  [UTF_SIZ + 1] = {        0,    0,  0x80,  0x800, 0x10000  };
static Rune  utfmax  [UTF_SIZ + 1] = { 0x10FFFF, 0x7F, 0x7FF, 0xFFFF, 0x10FFFF };


typedef enum {
    CS_GRAPHIC0,
    CS_GRAPHIC1,
    CS_UK,
    CS_USA,
    CS_MULTI,
    CS_GER,
    CS_FIN
} Charset;

typedef struct {
    short attr;  /* attribute flags */
    short fg;    /* foreground color; cache index */
    short bg;    /* background color; cache index */
    short row;
    short col;
} StackCursor;

typedef struct {
    uint row;
    uint col;
} Cell;

typedef struct {
    uint attr;  /* attribute flags */
    uint fg;    /* foreground color; cache index */
    uint bg;    /* background color; cache index */
    Cell p;
} TermCursor;

typedef struct {
    Cell ob;  /* original coordinates of the beginning of the selection */
    Cell oe;  /* original coordinates of the end of the selection */
    Cell nb;  /* normalized coordinates of the beginning of the selection */
    Cell ne;  /* normalized coordinates of the end of the selection */
} Selection;

#ifdef FEATURE_TITLE
typedef struct _Title {
    struct _Title *next;
    const char *val;
} Title;
#endif  /* FEATURE_TITLE */

/* Internal representation of the screen */
typedef struct {
    Cell size;               /* nb columns and rows */
    Line *line;              /* screen */
    Line *alt;               /* alternate screen */
    int *dirty;              /* dirtyness of lines (boolean) */
    int *tabs;               /* boolean */
    TermCursor c;            /* cursor */
    StackCursor cstack [2];  /* cursor stack */
    Cell oc;                 /* old cursor column and row */
    uint top, bottom;        /* top/bottom scroll limit */
    TermFlags flags;         /* terminal flags */
    byte trantbl [4];        /* charset table translation */
    int charset;             /* current charset */
    int icharset;            /* selected charset for sequence */
    Rune lastu;              /* last printed char outside of sequence, 0 if control */
    Selection sel;           /* selection */
#ifdef FEATURE_TITLE
    Title *titles;
    Title *icontitles;
#endif  /* FEATURE_TITLE */
} Term;

/* CSI Escape sequence structs */
/* ESC '[' [[ [<priv>] <arg> [;]] <mode> [<mode>]] */
typedef struct {
    char mode [2];
    /* raw string */
    char buf [ESC_BUF_SIZ];  /* raw string */
    size_t len;              /* raw string length */
    /* arguments */
    int args [ESC_ARG_SIZ];
    uint narg;               /* args # */
} CSIEscape;

/* STR Escape sequence structs */
/* ESC type [[ [<priv>] <arg> [;]] <mode>] ESC '\' */
typedef struct {
    char type;             /* ESC type ... */
    Thunk t;
    /* arguments */
    char *args [STR_ARG_SIZ];
    uint narg;             /* args # */
} STREscape;


static void execsh(const char **argv, uint argn);
static void stty(const char **argv, uint argn);
static void sigchld(int);

/* tty */
static void tty_write_raw (const char *, uint n);

/* OSC */
static void osc_color_response(int index, int id);
static int osc_handle (void);

/* CSI */
static void csi_push_icon_title (void);
static void csi_push_title (void);
static void csi_pop_icon_title (void);
static void csi_pop_title (void);

static int csi_handle0 (void);
static int csi_handle1 (int arg0);
static int csi_handle1_optional (int arg0);
static int csi_handle2_optional (int arg0, int arg1);
static void csi_handle (void);

static void csi_parse (void);
static void csi_reset (void);
static void csi_verbose (FILE *file);

/* ESC */
static const char * esc_type_to_string (uchar ascii);
static int esc_handle (uchar);

static void str_verbose (FILE *file);
static void str_handle (void);
static void str_parse (void);
static void str_reset (void);

/* Line management */
static void tline_dump (Line line);
static int tline_clear (Line line, uint row, uint col1, uint col2, int sel);
static int tline_is_attr (Line line, GlyphAttribute attr);
static uint tline_len (Line line);
static void tline_new (int);
static void tline_verbose (Line line);
static int tline_snap_prev (uint row);
static int tline_snap_next (uint row);
static void tline_snap_word_prev (uint *col, uint *row);
static void tline_snap_word_next (uint *col, uint *row);

/* region */
static void tregion_clear (uint, uint, uint, uint);
static void tregion_draw (uint, uint, uint, uint);
static void tregion_verbose (void);
static int tregion_is_sel (void);

/* cursor */
static void tcursor_load (void);
static void tcursor_save (void);
static void tcursor_stack (int set);

/* etc */
static void t_printer (char *, size_t);
static void t_dump (void);
static void t_delete_char (uint n);
static void t_delete_line (uint n);
static void t_insert_blank (uint n);
static void t_insert_blank_line (uint n);
static void t_move_to (uint col, uint row);
static void t_movea_to (uint col, uint row);
static void t_put_next_tab (uint n);
static void t_put_prev_tab (uint n);
static void t_putc (Rune);
static void t_reset (void);
static void t_scroll_up (uint orig, uint n);
static void t_scroll_down  (uint orig, uint n);
static void t_set_attr (void);
static void t_set_char (Rune, uint col, uint row);
static void t_set_dirt (uint top, uint bottom);
static void t_set_scroll  (uint top, uint bottom);
static void t_swap_screen (void);
static void t_set_mode (int);
static uint t_write (const char *buf, uint len, int);
static void t_full_dirt (void);
static void t_control_code (uchar );
static void t_dec_test (char );
static void t_def_utf8 (char);
static int t_def_color (const int **, int *, uint);
static int t_def_color_rgb (const int **, int *, uint);
static int t_def_color_index (const int **, int *, uint);
static void t_def_tran (char);
static void t_str_sequence (uchar);

/* selection */
static void sel_normalize (void);
static void sel_scroll (int, int);
static void sel_snap_next (uint *, uint *);
static void sel_snap_prev (uint *, uint *);
static void tsel_dump (void);

/* utf8 */
static uint utf8_decode (const char *, uint len, Rune *);
static Rune utf8_decode_byte (uchar, uint *ret);
static size_t utf8_length (Rune);

#ifdef ALLOW_WINDOW_OPS
static char *base64dec(const char *);
static char base64dec_getc(const char **);
#endif  /* ALLOW_WINDOW_OPS */

static int x_write (int, const char *, uint);

/* title fns and structures */
#ifdef FEATURE_TITLE
static Title * titles_push (Title *list, const char *val);
static Title * titles_pop (Title *list, const char **ret);
static void titles_free (Title *list);
static void title_free (Title *t);
#endif  /* FEATURE_TITLE */

/* sync update fns */
#ifdef FEATURE_SYNC_UPDATE
static void tsu_begin (void);
#endif

/* Globals */
static Term term;
static CSIEscape csiescseq;
static STREscape strescseq;
static int iofd = 1;
static int cmdfd;
static pid_t pid;

/* sync update globals */
#ifdef FEATURE_SYNC_UPDATE
static struct timespec tsu_stamp;
#endif


/*
 * Code
 */

#ifdef FEATURE_TITLE
Title *
titles_push (Title *list, const char *val)
{
    Title *title;
    
    /* new container */
    title = x_malloc (sizeof (Title));
    title->val = val;  /* string buffer is allocated already */
    
    /* prepend */
    title->next = list;
    return title;
}

Title *
titles_pop (Title *list, const char **ret)
{
    Title *next;

    /* free the container */
    *ret = list->val;
    next = list->next;
    free (list);

    return next;
}

void
title_free (Title *t)
{
    /* free string */
    free ((char *) t->val);
    /* free container */
    free (t);
}

void
titles_free (Title *list)
{
    Title *next;
    
    while ( list != NULL ) {
        next = list->next;
        title_free (list);
        list = next;
    }
}
#endif  /* FEATURE_TITLE */

#ifdef FEATURE_SYNC_UPDATE
void
tsu_begin (void)
{
    if ( clock_gettime (CLOCK_MONOTONIC, &tsu_stamp) == 0 )
        tty_sync_update_begin ();
}

int
tsu_clock (void)
{
    struct timespec now;

    if ( clock_gettime (CLOCK_MONOTONIC, &now) != 0 ||
         TIMEDIFF (now, tsu_stamp) < SYNC_TIMEOUT )
        return 1;
   
    tty_sync_update_end ();
    return 0;
}
#endif  /* FEATURE_SYNC_UPDATE */

int
x_write (int fd, const char *s, uint len)
{
    uint aux = len;
    int ret;

    while ( len != 0 ) {
        ret = write (fd, s, len);
        if ( ret < 0 )
            return ret;
       
        len -= ret;
        s += ret;
    }
    return aux;
}

void *
x_malloc (uint len)
{
    void *p;

    p = malloc (len);
    if ( p == NULL ) {
        error (msg_out_of_memory, strerror(errno));
        die ();
        /* NOP */
    }
    return p;
}

void *
x_realloc (void *p, uint len)
{
    p = realloc (p, len);
    if ( p == NULL ) {
        error (msg_out_of_memory, strerror(errno));
        die ();
        /* NOP */
    }
    return p;
}

/*
 * UTF8
 */

/* ATTN: $len can't be zero!
 * explanation: this fn is usually called with $len = UTF_SIZ or in
 * the loop where we checks the buffer length. */
uint
utf8_decode (const char *c, uint len, Rune *u)
{
    Rune decoded;
    uint ret, type, i;

    *u = UTF_INVALID;

    decoded = utf8_decode_byte ((uchar) *c, &ret);
    if ( !BETWEEN (ret, 1, UTF_SIZ) )
        return 1;
    
    if ( len < ret )
        return 0;
    
    for ( i = 1; i < ret; i++ ) {
        decoded = (decoded << 6) |
                   utf8_decode_byte ((uchar) *++c, &type);
        if ( type != 0 )
            return i;
    }

    /* utf8 validate */
    if ( BETWEEN  (decoded, utfmin [ret], utfmax [ret]) &&
         !BETWEEN (decoded, 0xD800,       0xDFFF) )
        *u = decoded;

    return ret;
}

Rune
utf8_decode_byte (uchar val, uint *ret)
{
    uint i;
    const uchar *pm, *pb;
    uchar m;

    for ( i = 0, pm = utfmask, pb = utfbyte;
          i <= UTF_SIZ;
          i++, pm++, pb++ ) {
        /* (val & utfmask [i]) == utfbyte [i] */
        m = *pm;
        if ( ( val & m ) == *pb ) {
            *ret = i;
            /* val & ~utfmask [i] */
            return val & ~m;
        }
    }

    *ret = UTF_SIZ + 1;
    return 0;
}

size_t
utf8_encode (Rune u, char *s)
{
    size_t ret, i;
    uchar mval, bval;
   
    /* utf8 validate */
    if ( !BETWEEN (u, *utfmin, *utfmax) ||
         BETWEEN  (u, 0xD800,  0xDFFF) )
        u = UTF_INVALID;

    /* get utf8 length */
    ret = utf8_length (u);
    if ( ret > UTF_SIZ )
        return 0;

    bval = *utfbyte;  /* utfbyte [0] */
    mval = *utfmask;  /* utfmask [0] */

    for ( i = ret - 1, s += i;
          i != 0;
          i--, s-- ) {
        /* encode s[i]: utfbyte [0] | (u & ~utfmask [0] */
        *s = bval | (u & ~mval);
        u >>= 6;
    }

    /* encode s[0]: utfbyte [len] | (u & ~utfmask [len] */
    *s = utfbyte [ret] | (u & ~utfmask [ret]);
    return ret;
}

size_t
utf8_length (Rune u)
{
    const Rune *pm;
    int i;

    for ( i = 1, pm = utfmax + 1;
          u > *pm;
          i++, pm++ )
        ;  /* NOP */

    return i;
}

/*
 * base64
 */
#ifdef ALLOW_WINDOW_OPS
static const char base64_digits[] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 62, 0, 0, 0,
    63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 0, 0, 0, -1, 0, 0, 0, 0, 1,
    2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
    22, 23, 24, 25, 0, 0, 0, 0, 0, 0, 26, 27, 28, 29, 30, 31, 32, 33, 34,
    35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

char
base64dec_getc(const char **src)
{
    while (**src && !isprint(**src))
        (*src)++;
    return **src ? *((*src)++) : '=';  /* emulate padding if string ends */
}

char *
base64dec(const char *src)
{
    size_t in_len = strlen(src);
    char *result, *dst;

    if (in_len % 4)
        in_len += 4 - (in_len % 4);

    result = dst = x_malloc ((in_len >> 2) * 3 + 1);
    while (*src) {
        int a = base64_digits[(unsigned char) base64dec_getc(&src)];
        int b = base64_digits[(unsigned char) base64dec_getc(&src)];
        int c = base64_digits[(unsigned char) base64dec_getc(&src)];
        int d = base64_digits[(unsigned char) base64dec_getc(&src)];

        /* invalid input. 'a' can be -1, e.g. if src is "\n" (c-str) */
        if (a == -1 || b == -1)
            break;

        *dst++ = (a << 2) | ((b & 0x30) >> 4);
        if (c == -1)
            break;

        *dst++ = ((b & 0x0f) << 4) | ((c & 0x3c) >> 2);
        if (d == -1)
            break;

        *dst++ = ((c & 0x03) << 6) | d;
    }

    *dst = '\0';
    return result;
}
#endif  /* ALLOW_WINDOW_OPS */

uint
tline_len (Line line)
{
    uint i;
   
    i = term.size.col;
    line += term.size.col - 1;
 
    if ( line->attr & ATTR_WRAP )
        return i;

    while ( i != 0 && line->rune == ' ' ) {
        i--;
        line--;
    }
    return i;
}

void
sel_start (uint col, uint row, uint snap)
{
    /* reset selection */
    if ( term.sel.oe.col != UINT_MAX )
        sel_clear ();

    /* selection flags */
    term.flags |= snap;
    
    if ( term_flag (MODE_ALTSCREEN) )
        term.flags |= SEL_ALTSCREEN;
   
    /* remember coordinations */ 
    term.sel.ob.col = col;
    term.sel.ob.row = row;
}

void
sel_extend (uint col, uint row, uint rect, int done)
{
    uint prev_rect, prev_nb_row, prev_ne_row;
    Cell prev_oe;

//    if ( term.sel.oe.col == -1 )
//        return;

    prev_oe.col = term.sel.oe.col;
    prev_oe.row = term.sel.oe.row;
    prev_rect = term_flag (SEL_RECT);

    prev_nb_row = term.sel.nb.row;
    prev_ne_row = term.sel.ne.row;
 
    /* normalize and remember coodinatios */
    term.sel.oe.col = col < term.size.col ? col : term.size.col - 1;
    term.sel.oe.row = row < term.size.row ? row : term.size.row - 1;

    sel_normalize ();

    /* set selection type */
    MODBIT (term.flags, rect, SEL_RECT);

    if ( prev_oe.col != term.sel.oe.col ||
         prev_oe.row != term.sel.oe.row ||
         prev_rect != rect ) {
        t_set_dirt  (MIN (term.sel.nb.row, prev_nb_row),
                  MAX (term.sel.ne.row, prev_ne_row));
    }
}
 
void
sel_normalize (void)
{
    /* column */
    if ( term_flag (SEL_RECT) ||                 /* rectangle? */
         term.sel.ob.row == term.sel.oe.row ) {  /* otherwise regular: single line? */
        if ( term.sel.ob.col < term.sel.oe.col ) {
            term.sel.nb.col = term.sel.ob.col;
            term.sel.ne.col = term.sel.oe.col;
        } else {
            term.sel.nb.col = term.sel.oe.col;
            term.sel.ne.col = term.sel.ob.col;
        }
    /* otherwise regular: multiple lines */
    } else if ( term.sel.ob.row < term.sel.oe.row ) {
        term.sel.nb.col = term.sel.ob.col;
        term.sel.ne.col = term.sel.oe.col;
    } else {
        term.sel.nb.col = term.sel.oe.col;
        term.sel.ne.col = term.sel.ob.col;
    }

    /* row */
    if ( term.sel.ob.row < term.sel.oe.row ) {
        term.sel.nb.row = term.sel.ob.row;
        term.sel.ne.row = term.sel.oe.row;
    } else {
        term.sel.nb.row = term.sel.oe.row;
        term.sel.ne.row = term.sel.ob.row;
    }

    /* snap selection */
    sel_snap_prev (&term.sel.nb.col, &term.sel.nb.row);
    sel_snap_next (&term.sel.ne.col, &term.sel.ne.row);
}

int
t_selected (uint col, uint row)
{
    uint cmin, cmax;

    if ( !tregion_is_sel () )
        return False;

    if ( !tline_sel_get_margin (row, &cmin, &cmax) )
        return False;

    return BETWEEN (col, cmin, cmax);
}

int
tregion_is_sel (void)
{
    uint altscreen;

    if ( term.sel.oe.col == UINT_MAX )
        return False;

    /* alt screen mode must be the same with selection's alt screen */
    altscreen = term_flag (MODE_ALTSCREEN | SEL_ALTSCREEN);
    return altscreen == 0 ||
           altscreen == (MODE_ALTSCREEN | SEL_ALTSCREEN); 
}

int
tline_sel_get_margin (uint row, uint *col1, uint *col2)
{
    if ( row < term.sel.nb.row ||
         row > term.sel.ne.row )
        return False;

    /* rectangle? */
    if ( term_flag (SEL_RECT) ) {
        *col1 = term.sel.nb.col;
        *col2 = term.sel.ne.col;
    } else {
        /* regular */
        *col1 = row == term.sel.nb.row ? term.sel.nb.col : 0;
        *col2 = row == term.sel.ne.row ? term.sel.ne.col : term.size.col - 1;
    }

    return True;
}

/* SNAP_WORD */
void
tline_snap_word_next (uint *col, uint *row)
{
    uint newcol, newrow, ccol, crow, linelen;
    int delim, prevdelim;
    TermGlyph *tg, *prevtg;
    Line *line;

    /* Snap around if the word wraps around at the end or beginning of a line. */
    crow = *row;
    line = term.line + crow;
    tg = *line;
    linelen = tline_len (tg);
    ccol = *col;
    tg += ccol;
    delim = ISDELIM (tg->rune);
    
    for ( newcol = ccol, newrow = crow; ; ccol = newcol, crow = newrow ) {
        prevdelim = delim;
        prevtg = tg;

        if ( ++newcol < term.size.col )
            tg++;
        else {
            if ( ++newrow == term.size.row )
                break;
           
            if ( !(tg->attr & ATTR_WRAP) )
                break;

            line++;
            tg = *line;
            linelen = tline_len (tg);
            newcol = 0; 
        }

        if ( newcol >= linelen )
            break;
        
        delim = ISDELIM (tg->rune);

        if ( (tg->attr & ATTR_WDUMMY) == 0 &&
             (delim != prevdelim ||
             (delim != 0 && tg->rune != prevtg->rune)) )
            break;
    }

    if ( ccol >= linelen )
        ccol = term.size.col - 1;

    *col = ccol;
    *row = crow;
} 

void
tline_snap_word_prev (uint *col, uint *row)
{
    uint newcol, newrow, ccol, crow, linelen;
    int delim, prevdelim;
    TermGlyph *tg, *prevtg;
    Line *line;

    /* Snap around if the word wraps around at the end or beginning of a line. */
    crow = *row;
    line = term.line + crow;
    tg = *line;
    linelen = tline_len (tg);
    ccol = *col;
    tg += ccol;
    delim = ISDELIM (tg->rune);
    
    for ( newcol = ccol, newrow = crow; ; ccol = newcol, crow = newrow ) {
        prevdelim = delim;
        prevtg = tg;

        if ( --newcol >= 0 )
            tg--;
        else {
            if ( --newrow < 0 )
                break;
           
            line--;
            tg = *line;
            linelen = tline_len (tg);
            newcol = term.size.col - 1; 
            tg += newcol;
            
            if ( !(tg->attr & ATTR_WRAP) )
                break;
        }

        if ( newcol >= linelen )
            break;
        
        delim = ISDELIM (tg->rune);

        if ( (tg->attr & ATTR_WDUMMY) == 0 &&
             (delim != prevdelim ||
             (delim != 0 && tg->rune != prevtg->rune)) )
            break;
    }

    if ( ccol > linelen )
        ccol = linelen;

    *col = ccol;
    *row = crow;
}

/* SNAP_LINE */
int
tline_snap_next (uint row)
{
    uint offset, max;
    TermGlyph *tg;
    Line *line;

    /*
     * Snap around if the the previous line or the current one
     * has set ATTR_WRAP at its end. Then the whole next or
     * previous line will be selected.
    */
    offset = term.size.col - 1;

    for ( line = term.line + row, max = term.size.row - 1; row < max; row++, line++ ) {
        tg = *line + offset;
        if ( (tg->attr & ATTR_WRAP) == 0 )
            break;
    }
    return row;
}

int
tline_snap_prev (uint row)
{
    uint offset;
    TermGlyph *tg;
    Line *line;

    /*
     * Snap around if the the previous line or the current one
     * has set ATTR_WRAP at its end. Then the whole next or
     * previous line will be selected.
    */
    offset = term.size.col - 1;

    for ( row--, line = term.line + row; row >= 0; row--, line-- ) {
        tg = *line + offset;
        if ( (tg->attr & ATTR_WRAP) == 0 )
            break;
    }
    return row;
}

void
sel_snap_next (uint *x, uint *y)
{
    if ( term_flag (SNAP_WORD) )
        tline_snap_word_next (x, y);
    else if ( term_flag (SNAP_LINE) ) {
        *x = term.size.col - 1;
        *y = tline_snap_next (*y);
    }
}

void
sel_snap_prev (uint *x, uint *y)
{
    if ( term_flag (SNAP_WORD) )
        tline_snap_word_prev (x, y);
    else if ( term_flag (SNAP_LINE) ) {
        *x = 0;
        *y = tline_snap_prev (*y);
    }
}

char *
sel_get (void)
{
    char *s, *ret;
    uint row, bufsize, prevcol, linelen;
    TermGlyph *tg, *last;
    Line *line;

    if ( term.sel.ob.col == UINT_MAX )
        return NULL;

    bufsize = (term.size.col + 1) * (term.sel.ne.row - term.sel.nb.row + 1) * UTF_SIZ;
    ret = s = x_malloc (bufsize);

    /* append every set & selected glyph to the selection */
    for ( row = term.sel.nb.row, line = term.line + row;
          row <= term.sel.ne.row;
          row++, line++ ) {
        /* is line empty? */
        tg = *line;
        linelen = tline_len (tg);
        if ( linelen-- == 0 ) {
            *s++ = '\n';
            continue;
        }
        last = tg;

        /* SEL_RECT? */
        if ( term_flag (SEL_RECT) ) {
            tg += term.sel.nb.col;
            prevcol = term.sel.ne.col;
        } else {
            /* SEL_REGULAR */
            if ( term.sel.nb.row == row )
                tg += term.sel.nb.col;
            
            prevcol = term.sel.ne.row == row ? term.sel.ne.col : term.size.col - 1;
        }

        if ( prevcol > linelen )
            prevcol = linelen;

        last += prevcol;
        while ( last >= tg && last->rune == ' ' )
            last--;

        for ( ; tg <= last; tg++ ) {
            if ( tg->attr & ATTR_WDUMMY )
                continue;

            s += utf8_encode (tg->rune, s);
        }

        /* Copy and pasting of line endings is inconsistent in the
         * inconsistent terminal and GUI world. The best solution
         * seems like to produce '\n' when something is copied from
         * st and convert '\n' to '\r', when something to be pasted
         * is received by st.
         * FIXME: Fix the computer world.
         */
        if ( (row < term.sel.ne.row || prevcol > linelen) &&
             ( (last->attr & ATTR_WRAP) == 0 || term_flag (SEL_RECT) ) )
            *s++ = '\n';
    }

    *s = 0;
    return ret;
}

void
sel_clear (void)
{
    term.sel.oe.col = UINT_MAX;
    term.flags &= ~SEL_MASK;

    t_set_dirt  (term.sel.nb.row, term.sel.ne.row);
}

void
x_exit (void)
{
    x_free ();
    exit (EXIT_SUCCESS);
}

void
die (void)
{
    x_free ();
    exit (EXIT_FAILURE);
}

void
execsh (const char **argv, uint argn)
{
    const struct passwd *pw;
    const char *prog, *sh;
    const char *cmd[3];

    errno = 0;
    if ((pw = getpwuid(getuid())) == NULL) {
        if (errno)
            error ("getpwuid: %s", strerror(errno));
        else
            error ("who are you?");
        die ();
        /* NOP */
    }

    sh = getenv ("SHELL");
    if ( sh == NULL )
        sh = *pw->pw_shell != '\0' ? pw->pw_shell : SHELL;
    
    if ( argn != 0 )
        prog = *argv;
    else {
        /* init the vector */
        argv = cmd;
#if defined (SCROLL)
        prog = SCROLL;
        *argv++ = prog;
#ifdef UTMP        
        *argv++ = SCROLL_UTMP;
#else
        *argv++ = sh;
#endif        
#elif defined (UTMP)
        prog = UTMP;
        *argv++ = prog;
#else
        prog = sh;
        *argv++ = prog;
#endif
        /* terminate and set the beginning */
        *argv = NULL;
        argv = cmd;
    }

    unsetenv ("COLUMNS");
    unsetenv ("LINES");
    unsetenv ("TERMCAP");

    setenv ("LOGNAME", pw->pw_name, 1);
    setenv ("USER",    pw->pw_name, 1);
    setenv ("SHELL",   sh,          1);
    setenv ("HOME",    pw->pw_dir,  1);
    setenv ("TERM",    termname,    1);

    signal (SIGCHLD, SIG_DFL);
    signal (SIGHUP,  SIG_DFL);
    signal (SIGINT,  SIG_DFL);
    signal (SIGQUIT, SIG_DFL);
    signal (SIGTERM, SIG_DFL);
    signal (SIGALRM, SIG_DFL);

    execvp (prog, (char * const* ) argv);

    x_free ();
    _exit (EXIT_FAILURE);
}

void
sigchld (int a)
{
    int stat;
    pid_t p;

    p = waitpid (pid, &stat, WNOHANG);
    if ( p < 0 ) {
        error ("waiting for pid %hd failed: %s", pid, strerror(errno));
        die ();
        /* NOP */
    }

    if (pid != p)
        return;

    if ( WIFEXITED (stat) && WEXITSTATUS (stat) ) {
        error ("child exited with status %d", WEXITSTATUS (stat));
        die ();
        /* NOP */
    }
    else if ( WIFSIGNALED(stat) ) {
        error ("child terminated due to signal %d", WTERMSIG (stat));
        die ();
        /* NOP */
    }
    x_exit ();
    /* NOP */
}

void
stty (const char **argv, uint argn)
{
    char cmd [_POSIX_ARG_MAX];
    const char *s;

    uint n = LEN (STTY_ARGS) - 1;  /* strlen */
    uint siz = LEN (cmd) - 1;  /* -1 due to '\0' */
    char *q = cmd;

    /* check size */
    if ( n > siz ) {
        error ("incorrect stty parameters");
        die ();
        /* NOP */
    }

    /* default stty args */
    memcpy (q, STTY_ARGS, n);
    q += n;
    siz -= n;

    while ( argn-- != 0 ) {
        s = *argv++;
        n = strlen (s);
        if ( n >= siz ) {  /* n >= siz ~ n + 1 > siz */
            error ("stty parameter length too long");
            die ();
            /* NOP */
        }
        /* space */
        *q++ = ' ';
        siz--;

        /* arg */
        memcpy (q, s, n);
        q += n;
        siz -= n;
    }

    /* terminate the string */
    *q = '\0';
    
    /* exec */
    if ( system (cmd) != 0 )
        error ("couldn't call stty: %s", strerror (errno));
}

int
tty_new (const char **argv, uint argn)
{
    int m, s;

    if ( a_io != NULL ) {
        term.flags |= MODE_PRINT;

        if ( *a_io == '\0' )
            /* FIXME: use macro or define one instead of hardcoded nr. */
            iofd = 1;
        else {
            iofd = open(a_io, O_WRONLY | O_CREAT, 0666);
            if (iofd < 0)
                error ("error opening %s:%s", a_io, strerror (errno));
        }
    }

    if ( a_line != NULL ) {
        cmdfd = open (a_line, O_RDWR);
        if ( cmdfd < 0 ) {
            error ("open line '%s' failed: %s", a_line, strerror (errno));
            die ();
            /* NOP */
        }

        dup2 (cmdfd, 0);
        stty (argv, argn);
        return cmdfd;
    }

    /* seems to work fine on linux, openbsd and freebsd */
    if ( openpty (&m, &s, NULL, NULL, NULL) < 0 ) {
        error ("openpty failed: %s", strerror(errno));
        die ();
        /* NOP */
    }

    pid = fork();
    switch ( pid ) {
        case -1:
            error ("fork failed: %s", strerror(errno));
            die ();
            /* NOP */

        case 0:
            close (iofd);
            setsid (); /* create a new process group */

            dup2 (s, 0);
            dup2 (s, 1);
            dup2 (s, 2);

            if ( ioctl (s, TIOCSCTTY, NULL) < 0 ) {
                error ("ioctl TIOCSCTTY failed: %s", strerror(errno));
                die ();
                /* NOP */
            }
            close(s);
            close(m);
#ifdef __OpenBSD__
            if (pledge("stdio getpw proc exec", NULL) == -1) {
                error ("pledge");
                die ();
                /* NOP */
            }
#endif
            execsh (argv, argn);
            break;

        default:
#ifdef __OpenBSD__
            if (pledge("stdio rpath tty proc", NULL) == -1) {
                error ("pledge");
                die ();
                /* NOP */
            }
#endif
            close(s);
            cmdfd = m;
            signal(SIGCHLD, sigchld);
            break;
    }

    return cmdfd;
}

uint
tty_read (void)
{
    static char buf [BUFSIZ];
    static uint buflen = 0;
    int ret;
    uint written;

    /* sync updates */
#ifdef FEATURE_SYNC_UPDATES    
    if ( tty_read_pending () ) {
        ret = 1;
        goto write;
    }
#endif  /* FEATURE_SYNC_UPDATES */

    /* append read bytes to unprocessed bytes */
    ret = read (cmdfd, buf + buflen, LEN (buf) - buflen);
    switch (ret) {
        case 0:
            x_exit ();
            /* NOP */

        case -1:
            error ("couldn't read from shell: %s", strerror(errno));
            die ();
            /* NOP */

        default:
            buflen += ret;
#ifdef FEATURE_SYNC_UPDATE            
write:
#endif            
            written = t_write  (buf, buflen, False);
            buflen -= written;
            /* keep any incomplete UTF-8 byte sequence for the next call */
            if ( buflen != 0 )
                memmove (buf, buf + written, buflen);
            return ret;
    }
}

void
tty_write (const char *s, uint n, int may_echo)
{
    const char *next;

    if ( may_echo && term_flag (MODE_ECHO) )
        t_write  (s, n, True);

    if (!term_flag (MODE_CRLF)) {
        tty_write_raw (s, n);
        return;
    }

    /* This is similar to how the kernel handles ONLCR for ttys */
    while ( n != 0 ) {
        if (*s == '\r') {
            next = s + 1;
            tty_write_raw ("\r\n", 2);
        } else {
            next = memchr (s, '\r', n);
            DEFAULT (next, s + n);
            tty_write_raw (s, next - s);
        }
        
        n -= next - s;
        s = next;
    }
}

void
tty_write_raw (const char *s, uint n)
{
    fd_set wfd, rfd;
    int ret;
    uint lim = 256;

    /* Remember that we are using a pty, which might be a modem line.
     * Writing too much will clog the line.  That's why we are doing
     * this dance.
     * FIXME: Migrate the world to Plan 9. */
    while ( n != 0 ) {
        FD_ZERO (&wfd);
        FD_ZERO (&rfd);

        FD_SET (cmdfd, &wfd);
        FD_SET (cmdfd, &rfd);

        /* Check if we can write. */
        if ( pselect (cmdfd + 1, &rfd, &wfd, NULL, NULL, NULL) < 0 ) {
            if (errno == EINTR)
                continue;
            error ("select failed: %s", strerror(errno));
            die ();
            /* NOP */
        }

        if (FD_ISSET (cmdfd, &wfd)) {
            /*
             * Only write the bytes written by tty_write() or the
             * default of 256. This seems to be a reasonable value
             * for a serial line. Bigger values might clog the I/O.
             */
            ret = write (cmdfd, s,
                   n < lim ? n : lim);
            if ( ret < 0 )
                goto write_error;

            if ( ret >= n )
                /* All bytes have been written. */
                break;
            /*
             * We weren't able to write out everything.
             * This means the buffer is getting full
             * again. Empty it.
             */
            if ( n < lim )
                lim = tty_read ();

            n -= ret;
            s += ret;
        }

        if (FD_ISSET (cmdfd, &rfd))
            lim = tty_read ();
    }
    return;

write_error:

    error ("write error on tty: %s", strerror(errno));
    die ();
    /* NOP */
}

void
tty_resize (int tw, int th)
{
    struct winsize w;

    w.ws_row = term.size.row;
    w.ws_col = term.size.col;
    w.ws_xpixel = tw;
    w.ws_ypixel = th;

    if ( ioctl (cmdfd, TIOCSWINSZ, &w) < 0 )
        error ("couldn't set window size: %s", strerror(errno));
}

void
tty_hangup (void)
{
    /* Send SIGHUP to shell */
    kill (pid, SIGHUP);
}

int
tline_is_attr (Line line, GlyphAttribute attr)
{
    uint i;

    for ( i = term.size.col; i != 0; i--, line++ ) {
        if ( line->attr & attr )
            return True;
    }
    return False;
}

int
tattr_set (GlyphAttribute attr)
{
    uint i;
    Line *line;

    for ( i = term.size.row, line = term.line; i != 0; i--, line++ ) {
        if ( tline_is_attr (*line, attr) )
            return True;
    }
    return False;
}

void
t_set_dirt (uint top, uint bottom)
{
    uint i;
    int *dirty;

    /* top */
    if ( top >= term.size.row )
        top = term.size.row - 1;

    /* bottom */
    if ( bottom >= term.size.row )
        bottom = term.size.row - 1;

    for ( i = top, dirty = term.dirty + i;
          i <= bottom;
          i++, dirty++ )
        *dirty = True;
}

void
tattr_dirtset (int attr)
{
    uint i;
    Line *line;
    int *dirty;

    for ( i = term.size.row, line = term.line, dirty = term.dirty;
          i != 0;
          i--, line++, dirty++ ) {
        if ( tline_is_attr (*line, attr) )
            *dirty = True;
    }
}

void
t_full_dirt (void)
{
    /* end sync update mode */
#ifdef FEATURE_SYNC_UPDATE    
    tty_sync_update_end ();
#endif
    t_set_dirt (0, term.size.row - 1);
}

void
tcursor_load (void)
{
    StackCursor *stack;

    /* src */
    stack = term.cstack;
    if ( term_flag (MODE_ALTSCREEN) )
        stack++;

    /* copy */
    term.c.attr = stack->attr;  /* attribute flags */
    term.c.fg = stack->fg;      /* foreground color; cache index */
    term.c.bg = stack->bg;      /* background color; cache index */
    term.c.p.col = stack->col;
    term.c.p.row = stack->row;

    /* set cursor */
    t_move_to (term.c.p.col, term.c.p.row);
}

void
tcursor_save (void)
{
    StackCursor *stack;

    /* dest */
    stack = term.cstack;
    if ( term_flag (MODE_ALTSCREEN) )
        stack++;

    /* copy */
    stack->attr = term.c.attr;  /* attribute flags */
    stack->fg = term.c.fg;      /* foreground color; cache index */
    stack->bg = stack->bg;      /* background color; cache index */
    stack->col = term.c.p.col;
    stack->row = term.c.p.row;
}

void
tcursor_stack (int set)
{
    if ( set )
        tcursor_save ();
    else
        tcursor_load ();
}

void
t_reset (void)
{
    int *tabs;
    int i;

    /* reset terminal flags */
    term.flags &= ~(MODE_MASK | CURSOR_MASK);
    term.flags |= MODE_WRAP | MODE_UTF8;

    /* init current cursor */
    term.c.attr = ATTR_NULL;
    term.c.p.col = 0;
    term.c.p.row = 0;
    term.c.fg = DEFAULT_FG;
    term.c.bg = DEFAULT_BG;

    /* tabs */
    memset (term.tabs, False, term.size.col * sizeof (int));
    for ( i = term.size.col - TAB_SPACES, tabs = term.tabs + TAB_SPACES;
          i > 0;
          i -= TAB_SPACES, tabs += TAB_SPACES )
        *tabs = True;
    
    /* init terminal */
    term.top = 0;
    term.bottom = term.size.row - 1;
    
    /* charset */
    memset (term.trantbl, CS_USA, sizeof (term.trantbl));
    term.charset = 0;

    /* swap */
    for ( i = 0; i < 2; i++ ) {
        t_move_to (0, 0);
        tcursor_save ();
        tregion_clear (0, 0, term.size.col - 1, term.size.row - 1);
        t_swap_screen ();
    }
}

void
t_new (uint col, uint row)
{
    memset (&term, 0, sizeof (Term));
    t_resize (col, row);
    t_reset ();
}

void
t_swap_screen (void)
{
    Line *swap;

    /* swap */
    swap = term.line;
    term.line = term.alt;
    term.alt = swap;
 
    term.flags ^= MODE_ALTSCREEN;

    t_full_dirt ();
}

void
t_scroll_down (uint orig, uint n)
{
    uint i;
    Line *line0, *line1;
    Line temp;

    i = term.bottom - orig + 1;
    if ( n > i )
        n = i;

    t_set_dirt  (orig, term.bottom - n);
    tregion_clear (0, term.bottom - n + 1, term.size.col - 1, term.bottom);

    for ( i = orig + n, line0 = term.line + term.bottom, line1 = line0 - n;
          i <= term.bottom;
          i++, line0--, line1-- ) {
        /* swap */
        temp = *line0;
        *line0 = *line1;
        *line1 = temp;
    }

    sel_scroll (orig, n);
}

void
t_scroll_up (uint orig, uint n)
{
    uint i;
    Line temp;
    Line *line0, *line1;

    i = term.bottom - orig + 1;
    if ( n > i )
        n = i;

    tregion_clear (0, orig, term.size.col - 1, orig + n - 1);
    t_set_dirt (orig + n, term.bottom);

    for ( i = orig + n, line0 = term.line + orig, line1 = line0 + n;
          i <= term.bottom;
          i++, line0++, line1++ ) {
        /* swap */
        temp = *line0;
        *line0 = *line1;
        *line1 = temp;
    }

    sel_scroll (orig, -n);
}

void
sel_scroll (int orig, int n)
{
    if ( term.sel.ob.col == UINT_MAX )
        return;

    if (BETWEEN (term.sel.nb.row, orig, term.bottom) != BETWEEN (term.sel.ne.row, orig, term.bottom))
        sel_clear ();
    else if (BETWEEN (term.sel.nb.row, orig, term.bottom)) {
        term.sel.ob.row += n;
        term.sel.oe.row += n;
        if ( term.sel.ob.row < term.top || term.sel.ob.row > term.bottom ||
             term.sel.oe.row < term.top || term.sel.oe.row > term.bottom )
            sel_clear ();
        else
            sel_normalize ();
    }
}

void
tline_new (int first_col)  /* boolean */
{
    uint row = term.c.p.row;

    if ( row == term.bottom )
        t_scroll_up (term.top, 1);
    else
        row++;

    t_move_to (first_col ? 0 : term.c.p.col, row);
}

void
csi_parse (void)
{
    char *s, *ns, *e;
    int *v;
    long int val;

    /* set argument # */
    csiescseq.narg = 0;

    /* priv */
    s = csiescseq.buf;
    if ( *s != '?' )
        term.flags &= ~CSI_PRIV;
    else {
        term.flags |= CSI_PRIV;
        s++;
    }

    e = csiescseq.buf + csiescseq.len;
    *e = '\0';

    for ( v = csiescseq.args; s < e; v++ ) {
        /* convert $s to long */
        ns = NULL;
        val = strtol (s, &ns, 10);
        if ( s == ns )
            val = 0;
        else if ( val == LONG_MAX || val == LONG_MIN )
            val = -1;

        /* add new argument */
        *v = val;
        csiescseq.narg++;

        /* next value */
        s = ns;
        if ( *s != ';' )
            break;
        
        /* check argument # */
        if ( csiescseq.narg == ESC_ARG_SIZ ) {
            warn ("CSI: too many arguments; ignored: %s", s);
            break;
        }

        /* s[0] is ';' so increment the pointer to the next
         * char */
        s++;
    }

    /* mode */
    csiescseq.mode [0] = *s++;
    csiescseq.mode [1] = s < e ? *s : '\0';
}

/* for absolute user moves, when decom is set */
void
t_movea_to (uint col, uint row)
{
    if ( term_flag (CURSOR_ORIGIN) )
        row += term.top;

    t_move_to (col, row);
}

void
t_move_to (uint col, uint row)
{
    /* flags */
    term.flags &= ~CURSOR_WRAPNEXT;

    /* x */
    if ( col >= term.size.col )
        col = term.size.col - 1;
    term.c.p.col = col;
 
    /* y */
    if ( term_flag (CURSOR_ORIGIN) )
        term.c.p.row = LIMIT (row, term.top, term.bottom);
    else {
        if ( row >= term.size.row )
            row = term.size.row - 1;
        term.c.p.row = row;
    }
}

void
t_set_char (Rune rune, uint col, uint row)
{
    char *s;
    TermGlyph *tg, *temp;

    static char *vt100_0 [62] = {                /* 0x41 - 0x7e */
        "↑", "↓", "→", "←", "█", "▚", "☃",       /* A - G */
        0, 0, 0, 0, 0, 0, 0, 0,                  /* H - O */
        0, 0, 0, 0, 0, 0, 0, 0,                  /* P - W */
        0, 0, 0, 0, 0, 0, 0, " ",                /* X - _ */
        "◆", "▒", "␉", "␌", "␍", "␊", "°", "±",  /* ` - g */
        "␤", "␋", "┘", "┐", "┌", "└", "┼", "⎺",  /* h - o */
        "⎻", "─", "⎼", "⎽", "├", "┤", "┴", "┬",  /* p - w */
        "│", "≤", "≥", "π", "≠", "£", "·",       /* x - ~ */
    };
    
    /*
     * The table is proudly stolen from rxvt.
     */
    if ( term.trantbl [term.charset] == CS_GRAPHIC0 &&
         BETWEEN (rune, 0x41, 0x7e) ) {
        s = vt100_0 [rune - 0x41];
        if ( s != NULL )
            utf8_decode (s, UTF_SIZ, &rune);
    }

    /* update glyph */
    temp = tg = term.line [row] + col;
    if ( tg->attr & ATTR_WIDE ) {
        if ( col + 1 < term.size.col ) {
            temp++;
            temp->rune = ' ';
            temp->attr &= ~ATTR_WDUMMY;
        }
    } else if ( tg->attr & ATTR_WDUMMY ) {
        temp--;
        temp->rune = ' ';
        temp->attr &= ~ATTR_WIDE;
    }

    /* copy cursor attributes */
    tg->rune = rune;
    tg->attr = term.c.attr;
    tg->fg = term.c.fg;
    tg->bg = term.c.bg;
 
    /* the line is dirty */
    term.dirty [row] = True;
} 

int
tline_clear (Line line, uint row, uint col1, uint col2, int sel)
{
    uint cmin, cmax;

    for ( line += col1; col1 <= col2; col1++, line++ ) {
        line->fg = term.c.fg;
        line->bg = term.c.bg;
        line->attr = 0;
        line->rune = ' ';
    }

    /* selection */
    if ( sel &&
         tline_sel_get_margin (row, &cmin, &cmax) &&
         cmin < col2 &&
         cmax > col1 ) {
        /* clear selection */
        sel_clear ();
        return True;
    }
    
    return False;
} 

void
tregion_clear (uint col1, uint row1, uint col2, uint row2)
{
    int temp;
    Line *line;
    int *dirty;

    /* x */
    if ( col1 > col2 ) {
        temp = col1;
        col1 = col2;
        col2 = temp;
    }
    /* limits */
    if ( col1 >= term.size.col )
        col1 = col2 = term.size.col - 1;  /* $col1 < $col2 therefore $col2 >= term.size.col */
    else if ( col2 >= term.size.col )
        col2 = term.size.col - 1;

    /* y */
    if ( row1 > row2 ) {
        temp = row1;
        row1 = row2;
        row2 = temp;
    }
    /* limits */
    if ( row1 >= term.size.row )
        row1 = row2 = term.size.row - 1;  /* $row1 < $row2 therefore $row2 >= term.size.row */
    else if ( row2 >= term.size.row )
        row2 = term.size.row - 1;

    /* selection */
    temp = tregion_is_sel ();

    /* clear */
    for ( dirty = term.dirty + row1, line = term.line + row1;
          row1 <= row2;
          row1++, dirty++, line++ ) {
        *dirty = True;

        if ( tline_clear (*line, row1, col1, col2, temp) )
            temp = False;
    }
}

void
t_delete_char (uint n)
{
    uint size;
    Line line;

    size = term.size.col - term.c.p.col;
    if ( n > size )
        n = size;

    line = term.line [term.c.p.row] + term.c.p.col;
    size = term.size.col - term.c.p.col - n;
 
    memmove (line, line + n, size * sizeof (TermGlyph));
    tregion_clear (term.size.col - n, term.c.p.row, term.size.col - 1, term.c.p.row);
}

void
t_insert_blank (uint n)
{
    uint size;
    Line line;

    size = term.size.col - term.c.p.col;
    if ( n > size )
        n = size;

    line = term.line [term.c.p.row] + term.c.p.col;
    size = term.size.col - term.c.p.col - n;
 
    memmove (line + n, line, size * sizeof (TermGlyph));
    tregion_clear (term.c.p.col, term.c.p.row, term.c.p.col + n - 1, term.c.p.row);
}

void
t_insert_blank_line (uint n)
{
    if ( BETWEEN (term.c.p.row, term.top, term.bottom) )
        t_scroll_down (term.c.p.row, n);
}

void
t_delete_line (uint n)
{
    if ( BETWEEN (term.c.p.row, term.top, term.bottom) )
        t_scroll_up (term.c.p.row, n);
}

int
t_def_color_rgb (const int **args, int *remain, uint id)
{
    int red, green, blue;
    const int *v;

    /* check argument # */
    red = *remain;
    red -= 3;
    if ( red <= 0 ) {
        error (msg_csi_arg_missing, id);
        return -1;
    }
    *remain = red;
 
    /* fetch arguments */
    v = *args;
    red   = *++v;
    green = *++v;
    blue  = *++v;
    *args = v;
 
    /* value */
    if ( !BETWEEN (red,   0, 255) ||
         !BETWEEN (green, 0, 255) ||
         !BETWEEN (blue,  0, 255) ) {
        error ("CSI(%d): bad RGB color: %d, %d, %d", id, red, green, blue);
        return -1;
    }

    /* load true color to cache */
    return x_color_load_rgb (red, green, blue);
}

int
t_def_color_index (const int **args, int *remain, uint id)
{
    int i;
    const int *v;

    /* check argument # */
    i = *remain;
    if ( --i == 0 ) {
        error (msg_csi_arg_missing, id);
        return -1;
    }
    *remain = i;

    /* fetch arguments */
    v = *args;
    i = *++v;
    *args = v;
 
    /* value */
    if ( !BETWEEN (i, 0, 255) ) {
        error ("CSI(%d): bad color index: %d", id, i);
        return -1;
    }
    
    return i;
}

/* Note $*args ($v) points to sequence beginning
 *      $*remain ($i) > 0 */
int
t_def_color (const int **args, int *remain, uint id)
{
    uint type;
    int ret, i;
    const int *v;

    /* The first sequence argument is handled so we need to move
     * the vector to next argument.  Therefore we're using `++v
     * instead of `v++.  The same applies to argument # */
    i = *remain;
    if ( --i == 0 ) {  /* no remaining arguments */
        error (msg_csi_arg_missing, id);
        return -1;
    }

    v = *args;
    type = *++v;

    switch ( type ) {
        case 2: /* direct color in RGB space */
            ret = t_def_color_rgb (&v, &i, id);
            break;

        case 5: /* indexed color */
            ret = t_def_color_index (&v, &i, id);
            break;

        case 0: /* implemented defined (only foreground) */
        case 1: /* transparent */
        case 3: /* direct color in CMY space */
        case 4: /* direct color in CMYK space */
        default:
            error ("CSI(%d): gfx attr %d unknown", id, type);
            ret = -1;
            break;
    }

    *args = v;    /* we'll increment the vector in the loop */
    *remain = i;  /* the same applies to index */
    return ret;
}

void
t_set_attr (void)
{
    int i;
    int attr;
    const int *args;

    for ( i = csiescseq.narg, args = csiescseq.args;
          i > 0;
          i--, args++ ) {
        attr = *args;

        switch (attr) {
            case 0:
                term.c.attr &= ~(
                    ATTR_BOLD       |
                    ATTR_FAINT      |
                    ATTR_ITALIC     |
                    ATTR_UNDERLINE  |
                    ATTR_BLINK      |
                    ATTR_REVERSE    |
                    ATTR_INVISIBLE  |
                    ATTR_STRUCK     );
                term.c.fg = DEFAULT_FG;
                term.c.bg = DEFAULT_BG;
                break;

            case 1:
                term.c.attr |= ATTR_BOLD;
                break;

            case 2:
                term.c.attr |= ATTR_FAINT;
                break;

            case 3:
                term.c.attr |= ATTR_ITALIC;
                break;

            case 4:
                term.c.attr |= ATTR_UNDERLINE;
                break;

            case 5: /* slow blink */
                /* FALLTHROUGH */
            case 6: /* rapid blink */
                term.c.attr |= ATTR_BLINK;
                break;

            case 7:
                term.c.attr |= ATTR_REVERSE;
                break;

            case 8:
                term.c.attr |= ATTR_INVISIBLE;
                break;

            case 9:
                term.c.attr |= ATTR_STRUCK;
                break;

            case 22:
                term.c.attr &= ~(ATTR_BOLD | ATTR_FAINT);
                break;

            case 23:
                term.c.attr &= ~ATTR_ITALIC;
                break;

            case 24:
                term.c.attr &= ~ATTR_UNDERLINE;
                break;

            case 25:
                term.c.attr &= ~ATTR_BLINK;
                break;

            case 27:
                term.c.attr &= ~ATTR_REVERSE;
                break;

            case 28:
                term.c.attr &= ~ATTR_INVISIBLE;
                break;

            case 29:
                term.c.attr &= ~ATTR_STRUCK;
                break;

            case 38:
                /* get color index or create true color */
                attr = t_def_color (&args, &i, 38);  /* $attr is used as a temp */
                if ( attr != -1 )
                    term.c.fg = attr;
                break;

            case 39:
                term.c.fg = DEFAULT_FG;
                break;

            case 48:
                attr = t_def_color (&args, &i, 48);  /* $attr is used as a temp */
                if ( attr != -1 )
                    term.c.bg = attr;
                break;

            case 49:
                term.c.bg = DEFAULT_BG;
                break;

            default:
                if ( BETWEEN (attr, 30, 37) )
                    term.c.fg = attr - 30;
                else if ( BETWEEN( attr, 40, 47) )
                    term.c.bg = attr - 40;
                else if ( BETWEEN (attr, 90, 97) )
                    term.c.fg = attr - 90 + 8;
                else if ( BETWEEN (attr, 100, 107) )
                    term.c.bg = attr - 100 + 8;
                else {
                    error ("CSI(%d): gfx attr unknown", attr);
                    csi_verbose (stderr);
                }
                break;
        }
    }
}

void
t_set_scroll (uint top, uint bottom)
{
    uint temp;

    /* swap */ 
    if ( top > bottom ) {
        temp = top;
        top = bottom;
        bottom = temp;
    }

    /* limit */
    if ( top >= term.size.row )
        top = bottom = term.size.row - 1;
    else if ( bottom >= term.size.row )
        bottom = term.size.row - 1;
 
    /* set */
    term.top = top;
    term.bottom = bottom;
}

void
t_set_mode (int set)
{
    int arg;
    uint i, alt;
    const int *v;

    for ( i = csiescseq.narg, v = csiescseq.args;
          i != 0;
          i--, v++ ) {
        arg = *v;
        if ( term_flag (CSI_PRIV) ) {
            switch (arg) {
                case 1: /* DECCKM -- Cursor key */
                    x_set_mode (set, MODE_APPCURSOR);
                    break;

                case 5: /* DECSCNM -- Reverse video */
                    x_set_mode (set, MODE_REVERSE);
                    break;

                case 6: /* DECOM -- Origin */
                    MODBIT(term.flags, set, CURSOR_ORIGIN);
                    t_movea_to (0, 0);
                    break;

                case 7: /* DECAWM -- Auto wrap */
                    MODBIT(term.flags, set, MODE_WRAP);
                    break;

                case 0:  /* Error (IGNORED) */
                case 2:  /* DECANM -- ANSI/VT52 (IGNORED) */
                case 3:  /* DECCOLM -- Column  (IGNORED) */
                case 4:  /* DECSCLM -- Scroll (IGNORED) */
                case 8:  /* DECARM -- Auto repeat (IGNORED) */
                case 18: /* DECPFF -- Printer feed (IGNORED) */
                case 19: /* DECPEX -- Printer extent (IGNORED) */
                case 42: /* DECNRCM -- National characters (IGNORED) */
                case 12: /* att610 -- Start blinking cursor (IGNORED) */
                    break;

                case 25: /* DECTCEM -- Text Cursor Enable Mode */
                    x_set_mode (!set, MODE_HIDE);
                    break;

                case 9:    /* X10 mouse compatibility mode */
                    x_set_pointer_motion (set);
                    x_set_mode (0, MODE_MOUSE);
                    x_set_mode (set, MODE_MOUSEX10);
                    break;

                case 1000: /* 1000: report button press */
                    x_set_pointer_motion (set);
                    x_set_mode (0, MODE_MOUSE);
                    x_set_mode (set, MODE_MOUSEBTN);
                    break;

                case 1002: /* 1002: report motion on button press */
                    x_set_pointer_motion (set);
                    x_set_mode (0, MODE_MOUSE);
                    x_set_mode (set, MODE_MOUSEMOTION);
                    break;

                case 1003: /* 1003: enable all mouse motions */
                    x_set_pointer_motion (set);
                    x_set_mode (0, MODE_MOUSE);
                    x_set_mode (set, MODE_MOUSEMANY);
                    break;

                case 1004: /* 1004: send focus events to tty */
                    x_set_mode (set, MODE_FOCUS);
                    break;

                case 1006: /* 1006: extended reporting mode */
                    x_set_mode (set, MODE_MOUSESGR);
                    break;

                case 1034:
                    x_set_mode (set, MODE_8BIT);
                    break;

                case 1049: /* swap screen & set/restore cursor as xterm */
                    if ( (a_flags & FlagAllowAltScreen) == 0 )
                        break;

                    tcursor_stack (set);
                    /* FALLTHROUGH */

                case 47: /* swap screen */
                case 1047:
                    if ( (a_flags & FlagAllowAltScreen) == 0 )
                        break;

                    alt = term_flag (MODE_ALTSCREEN);
                    if (alt)
                        tregion_clear (0, 0, term.size.col - 1, term.size.row - 1);
                    if (set ^ alt) /* set is always 1 or 0 */
                        t_swap_screen ();
                    if (arg != 1049)
                        break;
                    /* FALLTHROUGH */

                case 1048:
                    tcursor_stack (set);
                    break;

                case 2004: /* 2004: bracketed paste mode */
                    x_set_mode (set, MODE_BRCKTPASTE);
                    break;
                /* Not implemented mouse modes. See comments there. */

                case 1001: /* mouse highlight mode; can hang the
                          terminal by design when implemented. */
                case 1005: /* UTF-8 mouse mode; will confuse
                          applications not supporting UTF-8
                          and luit. */
                case 1015: /* urxvt mangled mouse mode; incompatible
                          and can be mistaken for other control
                          codes. */
                    break;

                default:
                    error( "CSI (%d): unknown private set/reset mode", arg);
                    break;
            }
        } else {
            switch (arg) {
                case 0:  /* Error (IGNORED) */
                    break;

                case 2:
                    x_set_mode (set, MODE_KBDLOCK);
                    break;

                case 4:  /* IRM -- Insertion-replacement */
                    MODBIT (term.flags, set, MODE_INSERT);
                    break;

                case 12: /* SRM -- Send/Receive */
                    MODBIT (term.flags, !set, MODE_ECHO);
                    break;

                case 20: /* LNM -- Linefeed/new line */
                    MODBIT (term.flags, set, MODE_CRLF);
                    break;

                default:
                    error ("CSI (%d): unknown set/reset mode", arg);
                    break;
            }
        }
    }
}

void
csi_push_icon_title (void)
{
    const char *val;

    /* get window title */
    val = x_get_icon_title ();
    if ( val == NULL )
        warn ("CSI: cannot push icon title to stack: undefined");
    else
        /* push the title to the stack */
        term.icontitles = titles_push (term.icontitles, val);
}

void
csi_push_title (void)
{
    const char *val;
    
    /* get window title */
    val = x_get_title ();
    if ( val == NULL )
        warn ("CSI: cannot push title to stack: undefined");
    else
        /* push the title to the stack */
        term.titles = titles_push (term.titles, val);
}

void
csi_pop_icon_title (void)
{
    const char *val;

    if ( term.icontitles == NULL )
        return;

    /* pop the title from the stack */
    term.icontitles = titles_pop (term.icontitles, &val);

    /* set title */
    if ( x_set_icon_title (val) != 0 )
        warn ("CSI: cannot pop icon title from stack");
    
    /* the container no longer exists so we are responsible for the
     * string */
    free ((char *) val);
}

void
csi_pop_title (void)
{
    const char *val;

    if ( term.titles == NULL )
        return;

    /* pop the title from the stack */
    term.titles = titles_pop (term.titles, &val);

    /* set title */
    if ( x_set_title (val) != 0 )
        warn ("CSI: cannot pop title from stack");

    /* the container no longer exists so we are responsible for the
     * string */
    free ((char *) val);
}

int
csi_handle0 (void)
{
    switch (csiescseq.mode [0]) {
        case 'h': /* SM -- Set terminal mode */
            t_set_mode (True);
            return True;

        case 'l': /* RM -- Reset Mode */
            t_set_mode (False);
            return True;

        case 'm': /* SGR -- Terminal attribute (color) */
            t_set_attr ();
            return True;

        case 's': /* DECSC -- Save cursor position (ANSI.SYS) */
            tcursor_save ();
            return True;

        case 'u': /* DECRC -- Restore cursor position (ANSI.SYS) */
            tcursor_load ();
            return True;
    }
    return False;
}

int
csi_handle1 (int arg0)
{
    switch (csiescseq.mode [0]) {
        case 'c': /* DA -- Device Attributes */
            if ( arg0 == 0 ) {
                tty_write (vtiden, strlen(vtiden), 0);
                return True;
            }
            break;

        case 'g': /* TBC -- Tabulation clear */
            switch (arg0) {
                case 0: /* clear current tab stop */
                    term.tabs [term.c.p.col] = False;
                    return True;

                case 3: /* clear all the tabs */
                    memset (term.tabs, False, term.size.col * sizeof(int));
                    return True;
            }
            break;;

        case 'i': /* MC -- Media Copy */
            /* handle */
            switch (arg0) {
            case 0:
                t_dump ();
                return True;

            case 1:
                tline_dump (term.line [term.c.p.row]);
                return True;

            case 2:
                tsel_dump ();
                return True;

            case 4:
                term.flags &= ~MODE_PRINT;
                return True;

            case 5:
                term.flags |= MODE_PRINT;
                return True;
            }
            break;

        case 'J': /* ED -- Clear screen */
            switch (arg0) {
                case 0: /* below */
                    tregion_clear (term.c.p.col, term.c.p.row, term.size.col - 1, term.c.p.row);
                    if (term.c.p.row < term.size.row - 1)
                        tregion_clear (0, term.c.p.row + 1, term.size.col - 1, term.size.row - 1);
                    return True;

                case 1: /* above */
                    if (term.c.p.row > 1)
                        tregion_clear (0, 0, term.size.col - 1, term.c.p.row - 1);
                    tregion_clear (0, term.c.p.row, term.c.p.col, term.c.p.row);
                    return True;

                case 2: /* all */
                    tregion_clear (0, 0, term.size.col - 1, term.size.row - 1);
                    return True;
            }
            break;

        case 'K': /* EL -- Clear line */
            switch (arg0) {
                case 0: /* right */
                    tregion_clear (term.c.p.col, term.c.p.row, term.size.col - 1, term.c.p.row);
                    return True;

                case 1: /* left */
                    tregion_clear (0, term.c.p.row, term.c.p.col, term.c.p.row);
                    return True;

                case 2: /* all */
                    tregion_clear (0, term.c.p.row, term.size.col - 1, term.c.p.row);
                    return True;
            }
            break;

        case 'n': /* DSR – Device Status Report (cursor position) */
            if (arg0 == 6) {
                char buf [40];

                /* $arg0 as a temp */
                arg0 = snprintf(buf, sizeof(buf), "\033[%i;%iR",
                        term.c.p.row + 1, term.c.p.col + 1);
                tty_write (buf, arg0, 0);
                return True;
            }
            break;

        case ' ':
            switch (csiescseq.mode [1]) {
                case 'q': /* DECSCUSR -- Set Cursor Style */
                    if ( x_set_cursor (arg0) == 0 )
                        return True;
                    break;
            }
            break;
    }
    return False;
}

int
csi_handle1_optional (int arg0)
{
    DEFAULT(arg0, 1);

    switch (csiescseq.mode [0]) {
        case '@': /* ICH -- Insert <n> blank char */
            t_insert_blank (arg0);
            return True;

        case 'A': /* CUU -- Cursor <n> Up */
            t_move_to (term.c.p.col, term.c.p.row - arg0);
            return True;

        case 'B': /* CUD -- Cursor <n> Down */
        case 'e': /* VPR --Cursor <n> Down */
            t_move_to (term.c.p.col, term.c.p.row + arg0);
            return True;

        case 'b': /* REP -- if last char is printable print it <n> more times */
            if ( term.lastu != 0 ) {
                while ( arg0-- > 0 )
                    t_putc (term.lastu);
            }
            return True;

        case 'C': /* CUF -- Cursor <n> Forward */
        case 'a': /* HPR -- Cursor <n> Forward */
            t_move_to (term.c.p.col + arg0, term.c.p.row);
            return True;

        case 'D': /* CUB -- Cursor <n> Backward */
            t_move_to (term.c.p.col - arg0, term.c.p.row);
            return True;

        case 'E': /* CNL -- Cursor <n> Down and first col */
            t_move_to (0, term.c.p.row + arg0);
            return True;

        case 'F': /* CPL -- Cursor <n> Up and first col */
            t_move_to (0, term.c.p.row - arg0);
            return True;

        case 'G': /* CHA -- Move to <col> */
        case '`': /* HPA */
            t_move_to (arg0 - 1, term.c.p.row);
            return True;

        case 'I': /* CHT -- Cursor Forward Tabulation <n> tab stops */
            t_put_next_tab (arg0);
            return True;

        case 'S': /* SU -- Scroll <n> line up */
            t_scroll_up (term.top, arg0);
            return True;

        case 'T': /* SD -- Scroll <n> line down */
            t_scroll_down (term.top, arg0);
            return True;

        case 'L': /* IL -- Insert <n> blank lines */
            t_insert_blank_line (arg0);
            return True;

        case 'M': /* DL -- Delete <n> lines */
            t_delete_line (arg0);
            return True;

        case 'X': /* ECH -- Erase <n> char */
            tregion_clear (term.c.p.col, term.c.p.row, term.c.p.col + arg0 - 1, term.c.p.row);
            return True;

        case 'P': /* DCH -- Delete <n> char */
            t_delete_char (arg0);
            return True;

        case 'Z': /* CBT -- Cursor Backward Tabulation <n> tab stops */
            t_put_prev_tab (arg0);
            return True;

        case 'd': /* VPA -- Move to <row> */
            t_movea_to (term.c.p.col, arg0 - 1);
            return True;
    }
    return False;
}

int
csi_handle2_optional (int arg0, int arg1)
{
    switch (csiescseq.mode [0]) {
        case 'H': /* CUP -- Move to <row> <col> */
        case 'f': /* HVP */
            DEFAULT (arg0, 1);
            DEFAULT (arg1, 1);
            t_movea_to (arg1 - 1, arg0 - 1);
            return True;

#ifdef FEATURE_TITLE
        case 't': /* title stack operations */
            switch (arg0) {
            case 22: /* pust current title on stack */
                switch (arg1) {
                case 0:
                    csi_push_icon_title ();
                    csi_push_title ();
                    return True;

                case 1:
                    csi_push_icon_title ();
                    return True;

                case 2:
                    csi_push_title ();
                    return True;
                }
                break;

            case 23: /* pop last title from stack */
                switch (arg1) {
                case 0:
                    csi_pop_icon_title ();
                    csi_pop_title ();
                    return True;

                case 1:
                    csi_pop_icon_title ();
                    return True;

                case 2:
                    csi_pop_title ();
                    return True;
                }
                break;
            }
            break;
#endif  /* FEATURE_TITLE */

        case 'r': /* DECSTBM -- Set Scrolling Region */
            if ( term_flag (CSI_PRIV) )
                break;

            DEFAULT (arg0, 1);
            DEFAULT (arg1, term.size.row);
            /* scroll */
            t_set_scroll (arg0 - 1, arg1 - 1);
            t_movea_to (0, 0);
            return True;
    }
    return False;
}

/* Some sequences have optional arguments: create new static fn
 * which will handle all possibilities which will be called from
 * csi_handleN function(s). Ta */
void
csi_handle (void)
{
    int arg0, arg1;
    int *v;

    /* reset esc mode */
    term.flags &= ~ESC_MASK;

    /* parse */
    csi_parse ();

    /* dump */
    if ( a_flags & FlagVerbose ) {
        verbose_info ();
        csi_verbose (stdout);
    }

    /* 1) first handle escape sequences with no arguments or
     * where we don't need to parse arguments */
    if ( csi_handle0 () )
        return;
    
    /* 2) then handle escape sequences with 1 optional argument */
    v = csiescseq.args;
    arg0 = *v;
    if ( csi_handle1_optional (arg0) )
        return;

    /* 3) then handle escape sequences with 2 optional arguments */
    arg1 = *++v;
    if ( csi_handle2_optional (arg0, arg1) )
        return;

    /* 3) then handle escape sequences with 1 mandatory argument */
    if ( csiescseq.narg == 0 )
        goto unknown;

    if ( csi_handle1 (arg0) )
        return;
 
unknown:

    /* warn */
    verbose_warn ();
    error_s ("CSI unhandled: ");
    csi_verbose (stderr);
}

void
csi_verbose (FILE *file)
{
    uint i;
    int *v;
    int comma;

    /* priv */
    fputs ("priv=", file);
    verbose_boolean (file, term_flag (CSI_PRIV));

    /* mode */
    fputs (", mode=[", file);
    verbose_color_begin (file, VerboseWhite);
    fputc (csiescseq.mode [0], file);
    fputc (' ', file);
    fputc (csiescseq.mode [1], file);
    verbose_color_end (file);
    
    /* arg # */
    fputs ("], args(", file);
    verbose_color_begin (file, VerboseWhite);
    fprintf (file, "%d", csiescseq.narg);
    verbose_color_end (file);
    fputs ("): ", file);
 
    /* args */
    for ( i = csiescseq.narg, v = csiescseq.args, comma = True;
          i != 0;
          i--, v++ ) {
        if ( comma )
            comma = False;
        else
            fputs (", ", file);

        verbose_color_begin (file, VerboseWhite);
        fprintf (file, "%d", *v);
        verbose_color_end (file);
    }

    fputc ('\n', file);
}

void
csi_reset (void)
{
    /* arg # will be cleared in csi_parse fn so we need to set the
     * length to zero only */
    csiescseq.len = 0;
}

void
osc_color_response (int index, int id)
{
    int n;
    char buf [32];
    char szidx [6];  /* strlen (";$$$$\0") */
    byte r, g, b;
    
    if ( !x_color_get (index, &r, &g, &b) ) {
        error ("OSC(%d): failed to fetch color: %d", id, index);
        return;
    }

    /* OSC (4)? */
    if ( id != 4 )
        *szidx = '\0';
    else
        snprintf (szidx, LEN (szidx), ";%d", index);

    n = snprintf(buf, sizeof (buf), "\033]%d%s;rgb:%02x%02x/%02x%02x/%02x%02x\007",
            id, szidx, r, r, g, g, b, b);
    tty_write (buf, n, 1);
}

int
osc_handle (void)
{
    int num0;
    char **v;
    char *arg1, *arg2;

    /* 1) handle OSC sequences with 1 argument */
/*      currently there is no such case otherwise just uncomment
        this condition and change the next one to strescseq.narg
        == 1 because this will handle 0
    if ( strescseq.narg == 0 )
        return 1;
 */
    /* 2) handle OSC sequences with 2 arguments */
    if ( strescseq.narg < 2 )
        return False;

    /* arg0 */
    v = strescseq.args;
    num0 = atoi (*v++);
    /* arg1 */
    arg1 = *v;  /* we don't increment the vector here */
 
    switch (num0) {
#ifdef FEATURE_TITLE
        case 0:
            x_set_title (arg1);
            x_set_icon_title (arg1);
            return True;

        case 1:
            x_set_icon_title (arg1);
            return True;

        case 2:
            x_set_title (arg1);
            return True;
#endif  /* FEATURE_TITLE */
 
        case 10:  /* foreground set */
            if ( strcmp(arg1, "?") == 0 )
                osc_color_response (DEFAULT_FG, 10);
            else if ( !x_color_set_name (DEFAULT_FG, arg1) )
                error ("OSC: invalid foreground color: %s", arg1);
            else
                t_draw (True);
            return True;

        case 11:  /* background set */
            if ( strcmp(arg1, "?") == 0 )
                osc_color_response (DEFAULT_BG, 11);
            else if ( !x_color_set_name (DEFAULT_BG, arg1) )
                error ("OSC: invalid background color: %s", arg1);
            else
                t_draw (True);
            return True;

        case 12:  /* cursor color */
            if ( strcmp(arg1, "?") == 0 )
                osc_color_response (DEFAULT_CS, 12);
            else if ( !x_color_set_name (DEFAULT_CS, arg1) )
                error ("OSC: invalid cursor color: %s", arg1);
            else
                t_draw (True);
            return True;

#ifdef ALLOW_WINDOW_OPS
        case 52:
            arg2 = base64dec(arg1);  /* $arg2 used as a temp */
            if (arg2 == NULL)
                error ("OSC: invalid base64: %s", arg2);
            else {
                x_set_sel (arg2);
                x_clip_copy ();
                /* we don't destroy the buffer becuase it's replaced
                 * with $xsel.primary */
            }
            return True;
#endif  /* ALLOW_WINDOW_OPS */

        case 104: /* color reset */
            num0 = atoi (arg1);  /* $num0 used as a temp! */
            if ( !x_color_set_name (num0, NULL) )
                error ("OSC: invalid color: idx=%d", num0);
            else
                /* TODO: if defaultbg color is changed, borders are
                 * dirty */
                t_draw (True);
            return True;
    }

    /* 3) handle OSC sequences with 3 arguments */
    if ( strescseq.narg == 2 )
        return False;

    /* arg2 */
    arg2 = *++v;

    switch (num0) {
        case 4:  /* color set */
            num0 = atoi (arg1);  /* $num0 used as a temp! */
            if ( strcmp (arg2, "?") == 0 )
                 osc_color_response (num0, 4);
            else if ( !x_color_set_name (num0, arg2) )
                 error ("OSC: invalid color: idx=%d, name=%s", num0, arg2);
            else
                 /* TODO: if defaultbg color is changed, borders are
                  * dirty */
                 t_draw (True);
            return True;
    }
    return False;
}

void
str_handle(void)
{
    char *arg0;
    char **v;

    /* parse */
    term.flags &= ~(ESC_STR_END | ESC_STR);
    str_parse ();

    if ( a_flags & FlagVerbose ) {
        verbose_info ();
        str_verbose (stdout);
    }

    /* 1) let's handle sequences with no arguments or
     * when we don't need to parse the arguments here */
    switch (strescseq.type) {
/* I commented the following code because I needed to see
 * all unhandled esc seq(s). */
#if 0        
        case '_': /* APC -- Application Program Command */
        case '^': /* PM -- Privacy Message */
            return;
#endif
        case ']': /* OSC -- Operating System Command */
            if ( osc_handle () )
                return;
            goto unknown;
    }

    /* 2) then sequences with 1 argument */
    if ( strescseq.narg == 0 )
        goto unknown;

    v = strescseq.args;
    arg0 = *v;

    switch (strescseq.type) {
#ifdef FEATURE_SYNC_UPDATE
         case 'P': /* DCS -- Device Control String */
            if ( strcmp (arg0, "=1s") == 0 ) {
                tsu_begin ();            /* BSU */
                return;
            }
            if ( strcmp (arg0, "=2s") == 0 ) {
                tty_sync_update_end ();  /* ESU */
                return;
            }
            goto unknown;
#endif  /* FEATURE_SYNC_UPDATE */

#ifdef FEATURE_TITLE
        case 'k': /* old title set compatibility */
            x_set_title (arg0);
            return;
#endif  /* FEATURE_TITLE */
    }
 
unknown:

    /* warn */
    verbose_warn ();
    error_s ("ESC unhandled: ");
    str_verbose (stderr);
}

void
str_parse (void)
{
    char c;
    char *s;
    char **v;
    
    /* clear argument # */
    strescseq.narg = 0;

    /* is buffer empty? */
    if ( strescseq.t.nelements == 0 )
        return;

    /* terminate the buffer */
    s = (char *) strescseq.t.items;
    s [strescseq.t.nelements] = '\0';

    for ( v = strescseq.args; ;v++ ) {
        /* next argument */
        *v = s;
        strescseq.narg++;

        /* find separator */
        for ( c = *s; c != ';'; c = *++s ) {
            if ( c == '\0' )
                return;
        }
        *s++ = '\0';
    
        /* STR_ARG_SIZ can't be zero, so we can check the
         * argments # here */
        if ( strescseq.narg == STR_ARG_SIZ ) {
            warn ("ESC: too many arguments; ignored: %s", s);
            return;
        }
    }
}

void
str_verbose (FILE *file)
{
    uint i;
    char **v;
    const char *s;
    int comma;
    char sz [MAX_HEX_SIZE];

    /* type */
    fputs ("type=", file);
    s = esc_type_to_string (strescseq.type);
    if ( s == NULL ) {
        s_hex (sz, strescseq.type);
        s = sz;
    }
    verbose_color (file, s, VerboseWhite);
 
    /* arg # */
    fputs (", args(", file);
    verbose_color_begin (file, VerboseWhite);
    fprintf (file, "%d", strescseq.narg);
    verbose_color_end (file);
    fputs ("): ", file);
 
    /* args */
    for ( i = strescseq.narg, v = strescseq.args, comma = True;
          i != 0;
          i--, v++ ) {
        if ( comma )
            comma = False;
        else
            fputs (", ", file);

        verbose_color (file, *v, VerboseWhite);
    }
    fputc ('\n', file);
}

void
t_init (void)
{
    term.sel.oe.col = UINT_MAX;

    thunk_create (&strescseq.t, STR_BUF_SIZ, sizeof (char));
}

void
str_reset (void)
{
    strescseq.t.items = x_realloc (strescseq.t.items, STR_BUF_SIZ);
    strescseq.t.alloc_size = STR_BUF_SIZ;
    strescseq.t.nelements = 0;
}

void
send_break (const Arg *arg)
{
    if ( tcsendbreak (cmdfd, 0) )
        error ("error sending break: %s", strerror (errno));
}

void
t_printer (char *s, size_t len)
{
    if ( iofd != -1 && x_write (iofd, s, len) < 0 ) {
        error ("error writing to output file: %s", strerror (errno));
        close (iofd);
        iofd = -1;
    }
}

void
print_toggle (const Arg *arg)
{
    term.flags ^= MODE_PRINT;
}

void
print_screen (const Arg *arg)
{
    t_dump ();
}

void
print_sel (const Arg *arg)
{
    tsel_dump ();
}

void
tsel_dump (void)
{
    char *ptr;

    ptr = sel_get ();
    if ( ptr != NULL ) {
        t_printer (ptr, strlen (ptr));
        free (ptr);
    }
}

void
tline_verbose (Line line)
{
    char buf [UTF_SIZ];
    uint count, ret;

    count = tline_len (line);
    while ( count-- != 0 ) {
        ret = utf8_encode (line->rune, buf);
        buf [ret] = '\0';
        verbose_s (buf);
        line++;
    }
    verbose_c ('|');
    verbose_newline ();
}

void
tregion_verbose (void)
{
    uint i;
    Line *line;

    for ( i = term.size.row, line = term.line;
          i != 0;
          i--, line++ )
        tline_verbose (*line);
}

void
tline_dump (Line line)
{
    char buf [UTF_SIZ];
    uint count, ret;

    /* tline_len returns value between 0 and term.size.col */
    count = tline_len (line);
    if ( count != 1 || line->rune != ' ' ) {
        while ( count-- != 0 ) {
            ret = utf8_encode (line->rune, buf);
            t_printer (buf, ret);
            line++;
        }
    }
    t_printer ("\n", 1);
}

void
t_dump (void)
{
    uint i;
    Line *line;

    for ( i = term.size.row, line = term.line;
          i != 0;
          i--, line++ )
        tline_dump (*line);
}

void
t_put_next_tab (uint n)
{
    uint col, max;
    int *pt;

    col = term.c.p.col;
    pt = term.tabs + col;
    max = term.size.col - 1;

    for ( ;; ) {
        if ( ++col == max )
            break;

        if ( *++pt && --n == 0 )
            break;
    }

    term.c.p.col = col;
}

void
t_put_prev_tab (uint n)
{
    uint col;
    int *pt;

    col = term.c.p.col;
    pt = term.tabs + col;

    for ( ;; ) {
        if ( --col == 0 )
            break;

        if ( *--pt && --n == 0 )
            break;
    }

    term.c.p.col = col;
}

void
t_def_utf8 (char ascii)
{
    if (ascii == 'G')
        term.flags |= MODE_UTF8;
    else if (ascii == '@')
        term.flags &= ~MODE_UTF8;
}

void
t_def_tran (char ascii)
{
    static char cs[] = "0B";
    static Charset vcs[] = { CS_GRAPHIC0, CS_USA };
    char *p;

    p = strchr (cs, ascii);
    if ( p == NULL )
        error ("ESC: unhandled charset: %c", ascii);
    else
        term.trantbl [term.icharset] = vcs [p - cs];
}

void
t_dec_test (char c)
{
    uint col, row;

    if (c == '8') { /* DEC screen alignment test. */
        for (col = 0; col < term.size.col; col++) {
            for (row = 0; row < term.size.row; row++)
                t_set_char ('E', col, row);
        }
    }
}

void
t_str_sequence (uchar c)
{
    switch (c) {
        case 0x90:   /* DCS -- Device Control String */
            c = 'P';
            break;

        case 0x9f:   /* APC -- Application Program Command */
            c = '_';
            break;

        case 0x9e:   /* PM -- Privacy Message */
            c = '^';
            break;

        case 0x9d:   /* OSC -- Operating System Command */
            c = ']';
            break;
    }

    str_reset ();
    strescseq.type = c;
    term.flags |= ESC_STR;
}

void
t_control_code (uchar ascii)
{
    switch (ascii) {
        case '\t':   /* HT */
            t_put_next_tab (1);
            return;

        case '\b':   /* BS */
            t_move_to (term.c.p.col - 1, term.c.p.row);
            return;

        case '\r':   /* CR */
            t_move_to (0, term.c.p.row);
            return;

        case '\f':   /* LF */
        case '\v':   /* VT */
        case '\n':   /* LF */
            /* go to first col if the mode is set */
            tline_new (term_flag(MODE_CRLF));
            return;

        case '\a':   /* BEL */
            if ( term_flag (ESC_STR_END) )
                /* backwards compatibility to xterm */
                str_handle ();
            else
                x_bell ();
            break;

        case '\033': /* ESC */
            csi_reset ();
            term.flags &= ~(ESC_CSI | ESC_ALTCHARSET | ESC_TEST);
            term.flags |= ESC_START;
            return;

        case '\016': /* SO (LS1 -- Locking shift 1) */
        case '\017': /* SI (LS0 -- Locking shift 0) */
            term.charset = 1 - (ascii - '\016');
            return;

        case '\032': /* SUB */
            t_set_char ('?', term.c.p.col, term.c.p.row);
            /* FALLTHROUGH */
        case '\030': /* CAN */
            csi_reset ();
            break;

        case '\005': /* ENQ (IGNORED) */
        case '\000': /* NUL (IGNORED) */
        case '\021': /* XON (IGNORED) */
        case '\023': /* XOFF (IGNORED) */
        case 0177:   /* DEL (IGNORED) */
            return;

        case 0x80:   /* TODO: PAD */
        case 0x81:   /* TODO: HOP */
        case 0x82:   /* TODO: BPH */
        case 0x83:   /* TODO: NBH */
        case 0x84:   /* TODO: IND */
            break;

        case 0x85:   /* NEL -- Next line */
            tline_new (True); /* always go to first col */
            break;

        case 0x86:   /* TODO: SSA */
        case 0x87:   /* TODO: ESA */
            break;

        case 0x88:   /* HTS -- Horizontal tab stop */
            term.tabs [term.c.p.col] = True;
            break;

        case 0x89:   /* TODO: HTJ */
        case 0x8a:   /* TODO: VTS */
        case 0x8b:   /* TODO: PLD */
        case 0x8c:   /* TODO: PLU */
        case 0x8d:   /* TODO: RI */
        case 0x8e:   /* TODO: SS2 */
        case 0x8f:   /* TODO: SS3 */
        case 0x91:   /* TODO: PU1 */
        case 0x92:   /* TODO: PU2 */
        case 0x93:   /* TODO: STS */
        case 0x94:   /* TODO: CCH */
        case 0x95:   /* TODO: MW */
        case 0x96:   /* TODO: SPA */
        case 0x97:   /* TODO: EPA */
        case 0x98:   /* TODO: SOS */
        case 0x99:   /* TODO: SGCI */
            break;

        case 0x9a:   /* DECID -- Identify Terminal */
            tty_write (vtiden, strlen(vtiden), 0);
            break;

        case 0x9b:   /* TODO: CSI */
        case 0x9c:   /* TODO: ST */
            break;

        case 0x90:   /* DCS -- Device Control String */
        case 0x9d:   /* OSC -- Operating System Command */
        case 0x9e:   /* PM -- Privacy Message */
        case 0x9f:   /* APC -- Application Program Command */
            t_str_sequence (ascii);
            return;
    }
    /* only CAN, SUB, \a and C1 chars interrupt a sequence */
    term.flags &= ~(ESC_STR_END | ESC_STR);
}

const char *
esc_type_to_string (uchar ascii)
{
    switch (ascii) {
        case '[':
            return esc_type_CSI;

        case '#':
            return esc_type_TEST;

        case '%':
            return esc_type_UTF8;

        case 'P':
            return esc_type_DSC;

        case '_':
            return esc_type_APC;

        case '^':
            return esc_type_PM;

        case ']':
            return esc_type_OSC;

        case 'n':
            return esc_type_LS2;

        case 'o':
            return esc_type_LS3;

        case '(':
            return esc_type_GZD4;

        case ')':
            return esc_type_G1D4;

        case '*':
            return esc_type_G2D4;

        case '+':
            return esc_type_G3D4;

        case 'D':
            return esc_type_IND;

        case 'E':
            return esc_type_NEL;

        case 'H':
            return esc_type_HTS;

        case 'M':
            return esc_type_RI;

        case 'Z':
            return esc_type_DECID;

        case 'c':
            return esc_type_RI;

        case '=':
            return esc_type_DECPAM;

        case '>':
            return esc_type_DECPNM;

        case '7':
            return esc_type_DECSC;

        case '8':
            return esc_type_DECRC;

        case '\\':
            return esc_type_ST;
    }

    return NULL;
}

/*
 * returns 1 when the sequence is finished and it hasn't to read
 * more characters for this sequence, otherwise 0
 */
int
esc_handle (uchar ascii)
{
    switch (ascii) {
        case '[':
            term.flags |= ESC_CSI;
            return False;

        case '#':
            term.flags |= ESC_TEST;
            return False;

        case '%':
            term.flags |= ESC_UTF8;
            return False;

        case 'P': /* DCS -- Device Control String */
        case '_': /* APC -- Application Program Command */
        case '^': /* PM -- Privacy Message */
        case ']': /* OSC -- Operating System Command */
        case 'k': /* old title set compatibility */
            t_str_sequence (ascii);
            return False;

        case 'n': /* LS2 -- Locking shift 2 */
        case 'o': /* LS3 -- Locking shift 3 */
            term.charset = 2 + (ascii - 'n');
            break;

        case '(': /* GZD4 -- set primary charset G0 */
        case ')': /* G1D4 -- set secondary charset G1 */
        case '*': /* G2D4 -- set tertiary charset G2 */
        case '+': /* G3D4 -- set quaternary charset G3 */
            term.icharset = ascii - '(';
            term.flags |= ESC_ALTCHARSET;
            return False;

        case 'D': /* IND -- Linefeed */
            if (term.c.p.row == term.bottom)
                t_scroll_up(term.top, 1);
            else
                t_move_to (term.c.p.col, term.c.p.row + 1);
            break;

        case 'E': /* NEL -- Next line */
            tline_new (True); /* always go to first col */
            break;

        case 'H': /* HTS -- Horizontal tab stop */
            term.tabs [term.c.p.col] = True;
            break;

        case 'M': /* RI -- Reverse index */
            if (term.c.p.row == term.top)
                t_scroll_down (term.top, 1);
            else
                t_move_to (term.c.p.col, term.c.p.row - 1);
            break;

        case 'Z': /* DECID -- Identify Terminal */
            tty_write (vtiden, strlen(vtiden), 0);
            break;

        case 'c': /* RIS -- Reset to initial state */
            t_reset ();
#ifdef FEATURE_TITLE            
            titles_free (term.titles);
            term.titles = NULL;
            x_set_title (a_title);
 
            titles_free (term.icontitles);
            term.icontitles = NULL;
            x_set_icon_title (a_title);
#endif  /* FEATURE_TITLE */
            x_colors_load_index ();
            break;

        case '=': /* DECPAM -- Application keypad */
            x_set_mode (1, MODE_APPKEYPAD);
            break;

        case '>': /* DECPNM -- Normal keypad */
            x_set_mode (0, MODE_APPKEYPAD);
            break;

        case '7': /* DECSC -- Save Cursor */
            tcursor_save ();
            break;

        case '8': /* DECRC -- Restore Cursor */
            tcursor_load ();
            break;

        case '\\': /* ST -- String Terminator */
            if ( term_flag (ESC_STR_END) )
                str_handle ();
            break;

        default:
            warn ("ESC: unhandled sequence 0x%02x '%c'",
                (uchar) ascii, isprint (ascii) ? ascii : '.');
            break;
    }

    return True;
}

void
t_putc (Rune rune)
{
    char c [UTF_SIZ];
    uint control, width, len, n;
    Line tg;

    control = ISCONTROL (rune);

    if ( rune < 127 || !term_flag (MODE_UTF8) ) {
        *c = rune;
        width = len = 1;
    } else {
        len = utf8_encode (rune, c);
        if ( !control && (width = wcwidth (rune)) == -1 )
            width = 1;
    }

    if ( term_flag (MODE_PRINT) )
        t_printer (c, len);

    /* STR sequence must be checked before anything else because
     * it uses all following characters until it receives a ESC,
     * a SUB, a ST or any other C1 control character. */
    if ( term_flag (ESC_STR) ) {
        if (rune == '\a' || rune == 030 || rune == 032 || rune == 033 || ISCONTROLC1 (rune)) {
            term.flags &= ~(ESC_START | ESC_STR);
            term.flags |= ESC_STR_END;
            goto check_control_code;
        }
        n = strescseq.t.nelements + len;
        if ( n >= strescseq.t.alloc_size ) { /* not >, but >= due to '\0' */
            /* Here is a bug in terminals. If the user never
             * sends some code to stop the str or esc command,
             * then st will stop responding.  But this is better
             * than silently failing with unknown characters. At
             * least then users will report back. In the case
             * users ever get fixed, here is the code: */
            /* term.flags &= ~ESC_MASK;
             * strhandle(); */
            if ( strescseq.t.alloc_size > ((SIZE_MAX - UTF_SIZ) >> 1) ) {
                warn ("ESC: too long");
                return;
            }
            thunk_double_size (&strescseq.t, n + 1);  /* +1 due to '\0' */
        }
        memcpy (((char *) strescseq.t.items) + strescseq.t.nelements, c, len);
        strescseq.t.nelements = n;
        return;
    }

check_control_code:
    /* Actions of control codes must be performed as soon they
     * arrive because they can be embedded inside a control sequence,
     * and they must not cause conflicts with sequences. */
    if (control) {
        t_control_code (rune);
        /* control codes are not shown ever */
        if ( (term.flags & ESC_MASK) == 0 )
            term.lastu = 0;
        return;
    }

    if ( term_flag (ESC_START) ) {
        if ( term_flag (ESC_CSI) ) {
            /* put char to CSI buffer */
            csiescseq.buf [csiescseq.len++] = rune;

            if ( BETWEEN (rune, 0x40, 0x7E) ||
                 csiescseq.len == LEN (csiescseq.buf) - 1 )  /* -1 due to '\0' */
                /* handle CSI esc seq */
                csi_handle ();

            return;
        }
        if ( term_flag (ESC_UTF8 ) )
            t_def_utf8 (rune);
        else if ( term_flag (ESC_ALTCHARSET) )
            t_def_tran (rune);
        else if ( term_flag (ESC_TEST) )
            t_dec_test (rune);
        else if ( !esc_handle (rune) )
            /* sequence already finished */
            return;

        /* reset esc mode */
        term.flags &= ~ESC_MASK;

        /* All characters which form part of a sequence are not printed */
        return;
    }

    if ( t_selected (term.c.p.col, term.c.p.row) )
        sel_clear ();

    tg = term.line [term.c.p.row] + term.c.p.col;
    if ( (term.flags & (MODE_WRAP | CURSOR_WRAPNEXT)) == (MODE_WRAP | CURSOR_WRAPNEXT) ) {
        tg->attr |= ATTR_WRAP;
        tline_new (True);
        tg = term.line [term.c.p.row] + term.c.p.col;
    }

    if ( term_flag (MODE_INSERT) && term.c.p.col + width < term.size.col )
        memmove (tg + width, tg, (term.size.col - term.c.p.col - width) * sizeof (TermGlyph));

    if (term.c.p.col + width > term.size.col) {
        tline_new (True);
        tg = term.line [term.c.p.row] + term.c.p.col;
    }

    t_set_char (rune, term.c.p.col, term.c.p.row);
    term.lastu = rune;

    if ( width == 2 ) {
        tg->attr |= ATTR_WIDE;
        if (term.c.p.col + 1 < term.size.col) {
            /* we don't use $tp anymore */
            tg++;
            tg->rune = '\0';
            tg->attr = ATTR_WDUMMY;
        }
    }

    if ( term.c.p.col + width < term.size.col )
        t_move_to (term.c.p.col + width, term.c.p.row);
    else
        term.flags |= CURSOR_WRAPNEXT;
}

uint
t_write (const char *buf, uint buflen, int show_ctrl)
{
    uint charsize, n;
    Rune rune;

    /* init */
#ifdef FEATURE_SYNC_UPDATE
    int sync = tty_sync_update ();
    tty_read_pending_end ();
#endif  /* FEATURE_SYNC_UPDATE */

    for ( n = 0;
          buflen != 0;
          n += charsize, buf += charsize, buflen -= charsize ) {
        if ( term_flag (MODE_UTF8) ) {
            /* process a complete utf8 char */
            charsize = utf8_decode (buf, buflen, &rune);
            if (charsize == 0)
                break;
        } else {
            rune = *buf;
            charsize = 1;
        }
        
        /* sync updates */ 
#ifdef FEATURE_SYNC_UPDATE
        if (sync && !tty_sync_update ()) {
            tty_read_pending_begin ();
            break;  /* ESU - allow rendering before a new BSU */
        }
#endif  /* FEATURE_SYNC_UPDATE */

        if ( show_ctrl && ISCONTROL (rune) ) {
            if ( rune & 0x80 ) {
                rune &= 0x7f;
                t_putc ('^');
                t_putc ('[');
            } else if ( rune != '\n' && rune != '\r' && rune != '\t' ) {
                rune ^= 0x40;
                t_putc ('^');
            }
        }
        t_putc (rune);
    }
    return n;
}

void
t_free (void)
{
    uint i;
    Line *l;

#ifdef FEATURE_TITLE    
    /* free title stack */
    titles_free (term.titles);
    titles_free (term.icontitles);
#endif  /* FEATURE_TITLE */

    /* line */
    for (i = term.size.row, l = term.line; i != 0; i--, l++)
        free(*l);

    /* alt */
    for (i = term.size.row, l = term.alt; i != 0; i--, l++ )
        free (*l);

    /* term */
    free (term.line);
    free (term.alt);
    free (term.dirty);
    free (term.tabs);

    /* strseq */
    thunk_free (&strescseq.t);

    /* fd */
    close (iofd);
}

void
t_resize (uint col, uint row)
{
    int n, minrow, mincol;
    int *tp, *ep;
    TermCursor c;
    Line *pl, *pa;
    
    if ( col == 0 || row == 0 ) {
        error ("cannot resize");
        return;
    }
    /*
     * slide screen to keep cursor where we expect it -
     * t_scroll_up would work here, but we can optimize to
     * memmove because we're freeing the earlier lines
     */
    minrow = term.c.p.row - row;
    for ( n = 0, pl = term.line, pa = term.alt;
          n <= minrow;
          n++, pl++, pa++ ) {
        free (*pl);
        free (*pa);
    }

    /* ensure that bottom src and dst are not NULL */
    if ( n != 0 ) {
        memmove (term.line, term.line + n, row * sizeof (Line));
        memmove (term.alt,  term.alt  + n, row * sizeof (Line));
    }

    for ( n += row, pl += row, pa += row;
          n < term.size.row;
          n++, pl++, pa++ ) {
        free (*pl);
        free (*pa);
    }
 
    /* resize to new height */
    term.line  = x_realloc (term.line,  row * sizeof (Line));
    term.alt   = x_realloc (term.alt,   row * sizeof (Line));
    term.dirty = x_realloc (term.dirty, row * sizeof (int));
    term.tabs  = x_realloc (term.tabs,  col * sizeof (int));

    /* resize each row to new width, zero-pad if needed */
    minrow = MIN (row, term.size.row);
   
    for ( n = 0, pl = term.line, pa = term.alt;
          n < minrow;
          n++, pl++, pa++ ) {
        *pl = x_realloc (*pl, col * sizeof (TermGlyph));
        *pa = x_realloc (*pa, col * sizeof (TermGlyph));
    }

    /* allocate any new rows */
    for ( ; n < row; n++, pl++, pa++ ) {
        *pl = x_malloc (col * sizeof (TermGlyph));
        *pa = x_malloc (col * sizeof (TermGlyph));
    }

    /* tabs */
    n = col - term.size.col;
    if ( n > 0 ) {
        tp = term.tabs + term.size.col;
        memset (tp, False, n * sizeof (int));

        while ( --tp > term.tabs && !*tp )
            /* nothing */ ;
        for ( tp += TAB_SPACES, ep = term.tabs + col;
              tp < ep;
              tp += TAB_SPACES )
            *tp = True;
    }
    
    /* update terminal size */
    mincol = MIN (col, term.size.col);
 
    term.size.col = col;
    term.size.row = row;
    
    /* reset scrolling region */
    t_set_scroll (0, row - 1);
    
    /* make use of the LIMIT in t_move_to  */
    t_move_to (term.c.p.col, term.c.p.row);
    
    /* Clearing bottom screens (it makes dirty all lines) */
    memcpy (&c, &term.c, sizeof (TermCursor));

    for ( n = 0; n < 2; n++ ) {
        if ( mincol < col && minrow > 0 )
            tregion_clear (mincol, 0, col - 1, minrow - 1);

        if ( col > 0 && row > minrow )
            tregion_clear (0, minrow, col - 1, row - 1);

        t_swap_screen ();
        tcursor_load ();
    }

    memcpy (&term.c, &c, sizeof (TermCursor));
}

void
tregion_draw (uint col1, uint row1, uint col2, uint row2)
{
    int sel;
    int *dirty;
    Line *line;

    /* selection */
    sel = tregion_is_sel ();

    for (dirty = term.dirty + row1, line = term.line + row1;
         row1 < row2;
         row1++, dirty++, line++) {
        if ( !*dirty )
            continue;
        
        /* redraw only dirty lines */
        *dirty = False;
        x_line_draw (*line, row1, col1, col2, sel);
    }
}

void
t_draw (int fulldirt)
{
    uint col, prev_col, prev_row;
    Line prev_tg, tg;

    if ( fulldirt )
        t_full_dirt ();

    if ( !x_is_mode_visible () )
        return;

//    info ("page: ");
//    tregion_verbose ();
    
    /* remember old valuse */
    prev_col = term.oc.col;
    prev_row = term.oc.row;
 
    /* adjust cursor position */
    if ( term.oc.col >= term.size.col )
        term.oc.col = term.size.col - 1;
    if ( term.oc.row >= term.size.row )
        term.oc.row = term.size.row - 1;
    
    prev_tg = term.line [term.oc.row] + term.oc.col;
    if ( prev_tg->attr & ATTR_WDUMMY ) {
        term.oc.col--;
        prev_tg--;
    }
    
    col = term.c.p.col;
    tg = term.line [term.c.p.row] + col;
    if ( tg->attr & ATTR_WDUMMY ) {
        col--;
        tg--;
    }
    
    /* draw */
    tregion_draw (0, 0, term.size.col, term.size.row);

    /* remove old cursor and draw new one */
    x_cursor_remove (prev_tg, term.oc.col, term.oc.row);
    x_cursor_draw (tg->rune, tg->attr, col, term.c.p.row);
    
    term.oc.col = col;
    term.oc.row = term.c.p.row;

    x_draw_finish ();
    
    if ( prev_col != term.oc.col ||
         prev_row != term.oc.row )
        x_im_spot (term.oc.col, term.oc.row);
}
