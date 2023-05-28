/* See LICENSE for license details. */

#include <errno.h>
#include <math.h>
#include <limits.h>
#include <locale.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/select.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include <X11/XKBlib.h>

#include "def.h"
#include "args.h"
#include "thunk.h"
#include "strutil.h"
#include "verbose.h"
#include "win.h"


/* X modifiers */
#define XK_ANY_MOD     UINT_MAX
#define XK_NO_MOD      0
#define XK_SWITCH_MOD  (1 << 13 | 1 << 14)

/* XEMBED messages */
#define XEMBED_FOCUS_IN   4
#define XEMBED_FOCUS_OUT  5

/* macros */
#define twin_flag(f)  (tw.flags & (f))

#define TRUERED(x)    (((x) & 0xff0000) >> 8)
#define TRUEGREEN(x)   ((x) & 0x00ff00)
#define TRUEBLUE(x)   (((x) & 0x0000ff) << 8)

#define FONT_MASK  (FontRegularBadSlant | FontRegularBadWeight | \
    FontItalicBadSlant | FontItalicBadWeight | FontBoldItalicBadSlant |\
    FontBoldItalicBadWeight | FontBoldBadSlant | FontBoldBadWeight)


typedef XftColor Color;
typedef XftDraw *Draw;
typedef XftGlyphFontSpec GlyphFontSpec;

typedef void (*EventHandler) (XEvent *);
typedef void (*ArgHandler) (const Arg *);



/* types used in config.h */
typedef struct {
    uint mod;
    KeySym keysym;
    ArgHandler func;
    const Arg arg;
} Shortcut;

typedef struct {
    uint mod;
    uint button;
    ArgHandler func;
    const Arg arg;
    uint release;
} MouseShortcut;

typedef struct {
    KeySym k;
    uint mask;
    char *s;
    /* three-valued logic variables: 0 indifferent, 1 on, -1 off */
    signed char appkey;     /* application keypad */
    signed char appcursor;  /* application cursor */
} Key;

/* Purely graphic info */
typedef struct {
    int tw, th;             /* tty width and height */
    int w, h;               /* twdow width and height */
    int ch;                 /* char height */
    int cw;                 /* char width  */
    TermWindowFlags flags;  /* term window state/mode and font flags */
    int cursor;             /* cursor style */
} TermWindow;

typedef struct {
    Display *dpy;
    Cursor cursor;
    Drawable buf;
    Draw draw;
    Window tw;
    struct {
        XIM xim;
        XIC xic;
        XVaNestedList spotlist;
        XPoint spot;
    } ime;
    int l, t;                /* left and top offset */
    int scr;
    int gm;                  /* geometry mask */
    Colormap cmap;
    GlyphFontSpec *specbuf;  /* font spec buffer used for rendering */
    Atom xembed, wmdeletewin, netwmname, netwmiconname, netwmpid;
    Visual *vis;
    XSetWindowAttributes attrs;
} XWindow;

typedef struct {
    char *primary;
    char *clipboard;
    Atom xtarget;
    struct timespec tclick1;
    struct timespec tclick2;
} XSelection;

/* Font structure */
typedef struct {
    XftFont *match;
    FcFontSet *set;
    FcPattern *pattern;
    int height;
    int width;
    int ascent;
    int descent;
    short lbearing;
    short rbearing;
} TermFont;

/* Font Ring Cache */
typedef enum {
    FRC_NORMAL,
    FRC_ITALIC,
    FRC_BOLD,
    FRC_ITALICBOLD
} FontcacheFlags;

typedef struct {
    XftFont *font;
    byte flags;
    Rune unicodep;
} Fontcache;

/* Dratwg Context */
typedef struct {
    Thunk clrcache;
    Thunk fntcache;
    TermFont rfont, bfont, ifont, ibfont;
    double usedfontsize;
    double defaultfontsize;
    GC gc;
} DC;


/* function definitions used in config.h */
static void clip_copy (const Arg *);
static void clip_paste (const Arg *);
static void numlock (const Arg *);
static void sel_paste (const Arg *);
static void zoom (const Arg *);
static void zoom_abs (const Arg *);
static void zoom_reset (const Arg *);
static void ttysend (const Arg *);

static int x_set_title_atom (const char *p, XTextProperty *prop, Atom atom);
static char * x_get_title_atom (Atom atom);

static inline ushort sixd_to_16bit (uint val);

/* glyph */
static TermFont * x_glyph_attr_to_font (GlyphAttribute attr, FontcacheFlags *retflags);
static TermFont * x_glyph_make_font_spec (XftGlyphFontSpec *ps, Rune rune, GlyphAttribute attr, TermFont *font, FontcacheFlags *retflags);
static int x_glyph_make_font_specs (XftGlyphFontSpec *, const TermGlyph *, int, int, int);
static void x_glyph_draw_font_specs (const XftGlyphFontSpec *, uint, uint, uint, GlyphAttribute, uint, uint);
static void x_glyph_draw (Rune rune, uint col, uint row, GlyphAttribute attr, uint fg, uint bg);

/* cursor */
static void x_cursor_draw_inactive (Color *drawcol, uint col, uint row);
static int x_cursor_draw_non_glyph (Color *drawcol, uint col, uint row);

static void x_clear (uint, uint, uint, uint);
static int x_geommask_to_gravity (int);
static int x_im_open (Display *);
static void x_im_instantiate (Display *, XPointer, XPointer);
static void x_im_destroy (XIM, XPointer, XPointer);
static int x_ic_destroy (XIC, XPointer, XPointer);
static int x_create (uint, uint);
static void cresize (uint, uint);
static void x_resize (uint, uint);
static void x_hints (void);

/* colors */
static void x_clrcache_free (void);
static int x_color_load_index (uint, const char *, Color *);
static int x_color_load_xterm (uint index, Color *color);
static int x_color_load_grey (uint index, Color *color);
static int x_color_load_value (uint red, uint green, uint blue, Color *color);
static int x_color_load_name (const char *name, Color *ret);
static void x_colors_reverse (void);
static int x_color_reverse (Color *c);

/* fonts */
static int x_font_load (TermFont *, FcPattern *);
static void x_fonts_load (double);
static void x_font_unload (TermFont *);
static void x_fonts_unload (void);

static void x_set_env (void);
static void x_set_urgency (int);
static uint evcol (XEvent *);
static uint evrow (XEvent *);

static void expose (XEvent *);
static void visibility (XEvent *);
static void unmap (XEvent *);
static void kpress (XEvent *);
static void cmessage (XEvent *);
static void resize (XEvent *);
static void focus (XEvent *);
static uint buttonmask (uint);
static int mouseaction (XEvent *, uint);
static void brelease (XEvent *);
static void bpress (XEvent *);
static void bmotion (XEvent *);
static void propnotify (XEvent *);
static void selnotify (XEvent *);

/* static void selclear_(XEvent *); */
static void sel_request (XEvent *);
static void sel_set (char *, Time);
static void mousesel (XEvent *, int);
static void mousereport (XEvent *);
static char *kmap (KeySym, uint);
static int match (uint, uint);

static void run (const char **argv, uint argn);
static void usage (void);
static void version (void );
static const char * get_prog_name (const char *name);


/* config.h for applying patches and the configuration. */
#include "config.h"

#define MAX_INDEX_CACHE  (LEN (colornames) + 256)


static EventHandler events [LASTEvent] = {
    [KeyPress]         = kpress,
    [ClientMessage]    = cmessage,
    [ConfigureNotify]  = resize,
    [VisibilityNotify] = visibility,
    [UnmapNotify]      = unmap,
    [Expose]           = expose,
    [FocusIn]          = focus,
    [FocusOut]         = focus,
    [MotionNotify]     = bmotion,
    [ButtonPress]      = bpress,
    [ButtonRelease]    = brelease,
/*
 * Uncomment if you want the selection to disappear when you select something
 * different in another twdow.
 */
/*    [SelectionClear] = selclear_, */
    [SelectionNotify]  = selnotify,
/*
 * PropertyNotify is only turned on when there is some INCR transfer happening
 * for the selection retrieval.
 */
    [PropertyNotify]   = propnotify,
    [SelectionRequest] = sel_request
};

/* Globals */
static DC dc = { 0 };
static XWindow xw = { 0 };
static XSelection xsel = { 0 };
static TermWindow tw;

static int __prev_button = 3;  /* button event on startup: 3 = release */


/*
 * Code
 */

void
clip_copy (const Arg *dummy)
{
    Atom clipboard;

    free (xsel.clipboard);
    xsel.clipboard = NULL;

    if ( xsel.primary != NULL ) {
        xsel.clipboard = s_dup (xsel.primary);
        clipboard = XInternAtom (xw.dpy, "CLIPBOARD", 0);
        XSetSelectionOwner (xw.dpy, clipboard, xw.tw, CurrentTime);
    }
}

void
clip_paste (const Arg *dummy)
{
    Atom clipboard;

    clipboard = XInternAtom (xw.dpy, "CLIPBOARD", 0);
    XConvertSelection (xw.dpy, clipboard, xsel.xtarget, clipboard, xw.tw, CurrentTime);
}

void
sel_paste (const Arg *dummy)
{
    XConvertSelection (xw.dpy, XA_PRIMARY, xsel.xtarget, XA_PRIMARY, xw.tw, CurrentTime);
}

void
numlock (const Arg *dummy)
{
    tw.flags ^= MODE_NUMLOCK;
}

void
zoom (const Arg *arg)
{
    Arg larg;

    larg.f = dc.usedfontsize + arg->f;
    zoom_abs (&larg);
}

void
zoom_abs (const Arg *arg)
{
    x_fonts_unload ();
    x_fonts_load (arg->f);
    cresize (0, 0);
    t_draw (True);
    x_hints ();
}

void
zoom_reset (const Arg *arg)
{
    Arg larg;

    if ( dc.defaultfontsize > 0 ) {
        larg.f = dc.defaultfontsize;
        zoom_abs (&larg);
    }
}

void
ttysend (const Arg *arg)
{
    tty_write (arg->s, strlen (arg->s), True);
}

uint
evcol (XEvent *e)
{
    int x = e->xbutton.x - BORDERPX;
    if ( x < 0 )
        x = 0;
    return x / tw.cw;
}

uint
evrow (XEvent *e)
{
    int y = e->xbutton.y - BORDERPY;
    if ( y < 0 )
        y = 0;
    return y / tw.ch;
}

void
mousesel (XEvent *e, int done)
{
    int seltype;
    uint state, col, row;
   
    state = e->xbutton.state & ~(Button1Mask | FORCE_MOUSE_MOD);
    seltype = match (SEL_RECTANGULAR_MASK, state) ? 0 : SEL_REGULAR;
    col = evcol (e);
    row = evrow (e);
        
    sel_extend (col, row, seltype, done);
    if ( done ) {
        sel_set (sel_get (), e->xbutton.time);
        x_clip_copy ();
    }
}

void
mousereport (XEvent *e)
{
    static uint prev_col, prev_row;

    char buf [40];
    int len;

    uint col = evcol (e);
    uint row = evrow (e);
    int button = e->xbutton.button;
    int state = e->xbutton.state;

    /* from urxvt */
    if (e->xbutton.type == MotionNotify) {
        if ( col == prev_col && row == prev_row )
            return;
        
        if ( !twin_flag (MODE_MOUSEMOTION | MODE_MOUSEMANY) )
            return;

        /* MOUSE_MOTION: no reporting if no button is pressed */
        if ( twin_flag (MODE_MOUSEMOTION) && __prev_button == 3 )
            return;

        button = __prev_button + 32;
        prev_col = col;
        prev_row = row;
    } else {
        if ( !twin_flag (MODE_MOUSESGR) && e->xbutton.type == ButtonRelease )
            button = 3;
        else {
            button -= Button1;
            if ( button >= 7 )
                button += 128 - 7;
            else if ( button >= 3 )
                button += 64 - 3;
        }
        if ( e->xbutton.type == ButtonPress ) {
            __prev_button = button;
            prev_col = col;
            prev_row = row;
        } else if ( e->xbutton.type == ButtonRelease ) {
            __prev_button = 3;
            /* MODE_MOUSEX10: no button release reporting */
            if ( twin_flag (MODE_MOUSEX10) )
                return;
            if (button == 64 || button == 65)
                return;
        }
    }

    if ( !twin_flag (MODE_MOUSEX10) )
        button += ((state & ShiftMask)   != 0 ? 4  : 0) +
                  ((state & Mod4Mask)    != 0 ? 8  : 0) +
                  ((state & ControlMask) != 0 ? 16 : 0);

    if ( twin_flag (MODE_MOUSESGR) )
        len = snprintf (buf, sizeof (buf), "\033[<%d;%d;%d%c",
               button, col + 1, row + 1,
               e->xbutton.type == ButtonRelease ? 'm' : 'M');
    else if ( col < 223 && row < 223 )
        len = snprintf (buf, sizeof (buf), "\033[M%c%c%c",
               32 + button, 32 + col + 1, 32 + row + 1);
    else
        return;

    tty_write (buf, len, False);
}

uint
buttonmask (uint button)
{
    return button == Button1 ? Button1Mask
         : button == Button2 ? Button2Mask
         : button == Button3 ? Button3Mask
         : button == Button4 ? Button4Mask
         : button == Button5 ? Button5Mask
         : 0;
}

int
mouseaction (XEvent *e, uint release)
{
    MouseShortcut *ms;
    uint i;

    /* ignore Button<N>mask for Button<N> - it's set on release */
    uint state = e->xbutton.state & ~buttonmask(e->xbutton.button);

    for ( i = LEN (mshortcuts), ms = mshortcuts; i != 0; i--, ms++ ) {
        if ( ms->release == release &&
             ms->button == e->xbutton.button &&
             ( match (ms->mod, state) || /* exact or forced */
               match (ms->mod, state & ~FORCE_MOUSE_MOD)) ) {
            ms->func (&ms->arg);
            return True;
        }
    }
    return False;
}

void
bpress (XEvent *e)
{
    struct timespec now;
    int snap;
    uint col, row;

    if ( twin_flag (MODE_MOUSE) &&
         !(e->xbutton.state & FORCE_MOUSE_MOD) ) {
        mousereport (e);
        return;
    }
    if ( mouseaction (e, 0) )
        return;

    if (e->xbutton.button == Button1) {
        /* If the user clicks below predefined timeouts specific snapping behaviour is exposed. */
        clock_gettime (CLOCK_MONOTONIC, &now);
        if ( TIMEDIFF (now, xsel.tclick2) <= CLICK_TRIPPLE_TIMEOUT )
            snap = SNAP_LINE;
        else if ( TIMEDIFF (now, xsel.tclick1) <= CLICK_DOUBLE_TIMEOUT )
            snap = SNAP_WORD;
        else
            snap = SNAP_NO;
       
        xsel.tclick2 = xsel.tclick1;
        xsel.tclick1 = now;

        col = evcol (e);
        row = evrow (e);
 
        sel_start (col, row, snap);
    }
}

void
propnotify (XEvent *e)
{
    XPropertyEvent *xpev;
    Atom clipboard = XInternAtom(xw.dpy, "CLIPBOARD", 0);

    xpev = &e->xproperty;
    if ( xpev->state == PropertyNewValue &&
         (xpev->atom == XA_PRIMARY ||
          xpev->atom == clipboard))
        selnotify(e);
}

void
selnotify (XEvent *e)
{
    ulong nitems, ofs, rem;
    int format;
    uchar *data, *last, *repl;
    Atom type, incratom, property = None;

    incratom = XInternAtom(xw.dpy, "INCR", 0);

    ofs = 0;
    if (e->type == SelectionNotify)
        property = e->xselection.property;
    else if (e->type == PropertyNotify)
        property = e->xproperty.atom;

    if (property == None)
        return;

    do {
        if (XGetWindowProperty(xw.dpy, xw.tw, property, ofs,
                   BUFSIZ >> 2, False, AnyPropertyType, &type,
                   &format, &nitems, &rem, &data)) {
            error ("clipboard allocation failed");
            return;
        }
        if (e->type == PropertyNotify && nitems == 0 && rem == 0) {
            /* If there is some PropertyNotify with no data, then
             * this is the signal of the selection owner that all
             * data has been transferred. We won't need to receive
             * PropertyNotify events anymore. */
            MODBIT (xw.attrs.event_mask, False, PropertyChangeMask);
            XChangeWindowAttributes (xw.dpy, xw.tw, CWEventMask, &xw.attrs);
        }
        if (type == incratom) {
            /* Activate the PropertyNotify events so we receive
             * when the selection owner does send us the next chunk of data. */
            MODBIT (xw.attrs.event_mask, True, PropertyChangeMask);
            XChangeWindowAttributes (xw.dpy, xw.tw, CWEventMask, &xw.attrs);

            /* Deleting the property is the transfer start signal. */
            XDeleteProperty (xw.dpy, xw.tw, (int)property);
            continue;
        }
        /* As seen in getsel:
         * Line endings are inconsistent in the terminal and GUI
         * world copy and pasting. When receiving some selection
         * data, replace all '\n' with '\r'.
         * FIXME: Fix the computer world. */
        repl = data;
        last = data + ((nitems * format) >> 3);  /* div 8 */

        while ((repl = memchr(repl, '\n', last - repl))) {
            *repl++ = '\r';
        }

        if (twin_flag(MODE_BRCKTPASTE) && ofs == 0)
            tty_write ("\033[200~", 6, False);

        tty_write ((char *)data, (nitems * format) >> 3, True);
        if (twin_flag(MODE_BRCKTPASTE) && rem == 0)
            tty_write ("\033[201~", 6, False);

        XFree(data);
        /* number of 32-bit chunks returned */
        ofs += (nitems * format) >> 5;  /* div 32 */
    } while ( rem > 0 );

    /* Deleting the property again tells the selection owner to send the next data chunk in the property. */
    XDeleteProperty(xw.dpy, xw.tw, (int)property);
}

void
x_clip_copy (void)
{
    clip_copy (NULL);
}
/*
void
selclear_(XEvent *e)
{
    selclear();
}
*/
void
sel_request (XEvent *e)
{
    XSelectionRequestEvent *xsre;
    XSelectionEvent xev;
    Atom xa_targets, string, clipboard;
    char *seltext;

    xsre = (XSelectionRequestEvent *) e;
    xev.type = SelectionNotify;
    xev.requestor = xsre->requestor;
    xev.selection = xsre->selection;
    xev.target = xsre->target;
    xev.time = xsre->time;
    if (xsre->property == None)
        xsre->property = xsre->target;

    /* reject */
    xev.property = None;

    xa_targets = XInternAtom (xw.dpy, "TARGETS", 0);
    if ( xsre->target == xa_targets ) {
        /* respond with the supported type */
        string = xsel.xtarget;
        XChangeProperty (xsre->display, xsre->requestor, xsre->property,
               XA_ATOM, 32, PropModeReplace, (uchar *) &string, 1);
        xev.property = xsre->property;
    } else if (xsre->target == xsel.xtarget || xsre->target == XA_STRING) {
        /* xith XA_STRING non ascii characters may be incorrect
         * in the requestor. It is not our problem, use utf8. */
        clipboard = XInternAtom(xw.dpy, "CLIPBOARD", 0);
        if (xsre->selection == XA_PRIMARY)
            seltext = xsel.primary;
        else if (xsre->selection == clipboard)
            seltext = xsel.clipboard;
        else {
            error ("unhandled clipboard selection 0x%lx", xsre->selection);
            return;
        }
        if (seltext != NULL) {
            XChangeProperty(xsre->display, xsre->requestor, xsre->property,
                   xsre->target, 8, PropModeReplace, (uchar *)seltext, strlen(seltext));
            xev.property = xsre->property;
        }
    }

    /* all done, send a notification to the listener */
    if ( !XSendEvent (xsre->display, xsre->requestor, 1, 0, (XEvent *) &xev) )
        error ("sending SelectionNotify event");
}

void
sel_set (char *str, Time t)
{
    if (!str)
        return;

    /* replace the buffer */
    free (xsel.primary);
    xsel.primary = str;

    XSetSelectionOwner (xw.dpy, XA_PRIMARY, xw.tw, t);
    if ( XGetSelectionOwner (xw.dpy, XA_PRIMARY) != xw.tw )
        sel_clear ();
}

void
x_set_sel (char *str)
{
    sel_set (str, CurrentTime);
}

void
brelease (XEvent *e)
{
    if ( twin_flag (MODE_MOUSE) &&
         !(e->xbutton.state & FORCE_MOUSE_MOD) ) {
        mousereport (e);
        return;
    }
    if ( mouseaction (e, 1) )
        return;

    if ( e->xbutton.button == Button1 )
        mousesel (e, True);
}

void
bmotion (XEvent *e)
{
    if ( twin_flag (MODE_MOUSE) &&
         !(e->xbutton.state & FORCE_MOUSE_MOD) ) {
        mousereport (e);
        return;
    }
    mousesel (e, False);
}

void
cresize (uint width, uint height)
{
    int col, row;

    if ( width != 0 )
        tw.w = width;
    if ( height != 0 )
        tw.h = height;

    col = (tw.w - (BORDERPX << 1)) / tw.cw;
    row = (tw.h - (BORDERPY << 1)) / tw.ch;
    col = MAX (1, col);
    row = MAX (1, row);

    t_resize (col, row);
    x_resize (col, row);
    tty_resize (tw.tw, tw.th);
}

void
x_resize (uint col, uint row)
{
    tw.tw = col * tw.cw;
    tw.th = row * tw.ch;

    XFreePixmap (xw.dpy, xw.buf);
    xw.buf = XCreatePixmap (xw.dpy, xw.tw, tw.w, tw.h,
                            DefaultDepth (xw.dpy, xw.scr));
    XftDrawChange (xw.draw, xw.buf);
    x_clear (0, 0, tw.w, tw.h);

    /* resize to new width */
    xw.specbuf = x_realloc (xw.specbuf, col * sizeof (GlyphFontSpec));
}

int
x_color_load_name (const char *name, Color *ret)
{
    if ( XftColorAllocName (xw.dpy, xw.vis, xw.cmap, name, ret) )
        return True;

    error ("could not allocate color: %s", name);
    return False;
} 

int
x_color_load_value (uint red, uint green, uint blue, Color *color)
{
    XRenderColor render;

    render.alpha = 0xffff;
    render.red   = red;
    render.green = green;
    render.blue  = blue;

    if ( XftColorAllocValue (xw.dpy, xw.vis, xw.cmap, &render, color) )
        return True;

    error ("could not allocate RGB color: %d, %d, %d", red, green, blue);
    return False;
}

ushort
sixd_to_16bit (uint val)
{
    if ( val == 0 )
        return 0;
    return val * 0x2828 + 0x3737;
}

/* $index must be < 6*6*6 */
int
x_color_load_xterm (uint index, Color *color)
{
    return x_color_load_value (sixd_to_16bit ((index / 36) % 6),
                               sixd_to_16bit ((index /  6) % 6),
                               sixd_to_16bit ( index       % 6),
                               color);
}

/* $index must be < 24 */
int
x_color_load_grey (uint index, Color *color)
{
    index *= 0x0a0a;
    index += 0x0808;
    
    return x_color_load_value (index, index, index, color);
}

int
x_color_load_index (uint idx, const char *name, Color *ret)
{
    /* $idx is unsigned int therefore we don't need to check < 0
     * condition.  The value must be < 256 + countof (colnames)
     * because we don't want to overwrite cache for true color entries */
    if ( idx >= MAX_INDEX_CACHE )
        return False;
   
    /* is name empty? */
    if ( name == NULL ) {
        /* base colors(16) */
        if ( idx < 16 )
            name = basecolornames [idx];
        else {
            /* same colors(216) as xterm */
            idx -= 16;
            if ( idx < 216 )
                return x_color_load_xterm (idx, ret);

            /* greyscale(24) */
            idx -= 216;
            if ( idx < 24 )
                return x_color_load_grey (idx, ret);

            /* custom colors */
            idx -= 24;
            name = colornames [idx];
        }
    }
    return x_color_load_name (name, ret);
}

void
x_clrcache_free (void)
{
    Color *c;
    uint i;

    for ( i = dc.clrcache.nelements, c = (Color *) dc.clrcache.items;
          i != 0;
          i--, c++ ) {
        /* deleted already? */
        if ( c->pixel == 0 )
            continue;

        /* free color */
        XftColorFree (xw.dpy, xw.vis, xw.cmap, c);

        /* mark the thunk as deleted will fail */
        c->pixel = 0;
    }

    /* clear # */
    dc.clrcache.nelements = 0;
}

int
x_color_reverse (Color *c)
{
    Color new_c;

    /* create reversed color */
    if ( !x_color_load_value (~c->color.red,
                              ~c->color.green,
                              ~c->color.blue,
                              &new_c) )
        return False;

    /* free current color */
    XftColorFree (xw.dpy, xw.vis, xw.cmap, c);
 
    /* and set new one */
    memcpy (c, &new_c, sizeof (Color));
    return True;
}

void
x_colors_reverse (void)
{
    Color *c, *fg;
    Color swap;
    int idx_save, idx_swap, idx_cur;
   
    if ( DEFAULT_FG < DEFAULT_BG ) {
        idx_save = DEFAULT_BG;
        idx_swap = DEFAULT_FG;
    } else {
        idx_save = DEFAULT_FG;
        idx_swap = DEFAULT_BG;
    }

    /* revers all true colors */
    for ( idx_cur = dc.clrcache.nelements, c = (Color *) dc.clrcache.items;
          idx_cur != 0;
          idx_cur--, c++ ) {
        if ( idx_cur == idx_save ) {
            /* copy the content to the temp */
            memcpy (&swap, c, sizeof (Color));
            /* remember the thunk */
            fg = c;
            continue;
        }

        if ( idx_cur == idx_swap ) {
            /* swap colors */
            memcpy (fg, c,     sizeof (Color));
            memcpy (c,  &swap, sizeof (Color));
            continue;
        }

        if ( !x_color_reverse (c) )
            break;
    }
}

int
x_color_load_rgb (uint red, uint green, uint blue)
{
    Color *new_c;
    uint ret;

    /* first allocate new thunk */
    ret = dc.clrcache.nelements;
    new_c = (Color *) thunk_alloc_next (&dc.clrcache);

    /* is reverse mode? */
    if ( twin_flag (MODE_REVERSE) ) {
    }

    /* create new color */
    if ( !x_color_load_value (
                red   << 8,
                green << 8,
                blue  << 8,
                new_c) ) {
        /* remove thunk */
        dc.clrcache.nelements--;
        return -1;
    }

    return ret;
} 

int
x_color_load_faint (uint idx)
{
    Color *new_c, *src;
    uint ret;

    /* first allocate new thunk */
    ret = dc.clrcache.nelements;
    new_c = (Color *) thunk_alloc_next (&dc.clrcache);

    /* init color */
    src = (Color *) dc.clrcache.items + idx;

    /* create new color */
    if ( !x_color_load_value (src->color.red   >> 1,
                              src->color.green >> 1,
                              src->color.blue  >> 1,
                              new_c) ) {
        /* remove thunk */
        dc.clrcache.nelements--;
        return -1;
    }

    return ret;
}

void
x_colors_load_index (void)
{
    int i;
    Color *c;
    const char **v;

    /* free color cache */
    x_clrcache_free ();

    /* base colors */
    for ( i = 0, c = (Color *) dc.clrcache.items, v = basecolornames;
          i < 16;
          i++, c++, v++ ) {
        if ( !x_color_load_name (*v, c) )
            goto quit;

        dc.clrcache.nelements++;
    }
    
    /* system colors */
    for ( i = 0; i < 216; i++, c++ ) {
        if ( !x_color_load_xterm (i, c) )
            goto quit;

        dc.clrcache.nelements++;
    }
    
    /* system colors */
    for ( i = 0; i < 24; i++, c++ ) {
        if ( !x_color_load_grey (i, c) )
            goto quit;

        dc.clrcache.nelements++;
    }

    /* custom colors */
    for ( i = 0, v = colornames;
          i < LEN (colornames);
          i++, c++, v++ ) {
        if ( !x_color_load_name (*v, c) )
            goto quit;

        dc.clrcache.nelements++;
    }

    return;

quit:
    die ();
    /* NOP */
}

int
x_color_get (uint idx, byte *r, byte *g, byte *b)
{
    Color *clr;

    /* $idx is unsigned int therefore we don't need to check < 0
     * and the value must be < $dc.col.nelements */
    if ( idx >= dc.clrcache.nelements )
        return False;

    clr = (Color *) dc.clrcache.items + idx;

    *r = clr->color.red   >> 8;
    *g = clr->color.green >> 8;
    *b = clr->color.blue  >> 8;
    return True;
}

int
x_color_set_name (uint idx, const char *name)
{
    Color src;
    Color *dst;

    /* we'll check the index in the follotwg fn */
    if ( !x_color_load_index (idx, name, &src) )
        return False;
    /* $idx is between <0, countof (colnames) + 256) */

    /* free current color */
    dst = (Color *) dc.clrcache.items + idx;
    XftColorFree (xw.dpy, xw.vis, xw.cmap, dst);
 
    /* and set new one */
    memcpy (dst, &src, sizeof (Color));
    return True;
}

/*
 * Absolute coordinates.
 */
void
x_clear (uint x1, uint y1, uint x2, uint y2)
{
    Color *c;

    /* we don't need to handle MODE_REVERSE because the colors are swapped in
     * this mode */
    c = (Color *) dc.clrcache.items + DEFAULT_BG;
    XftDrawRect (xw.draw, c, x1, y1, x2 - x1, y2 - y1);
}

void
x_hints (void)
{
    XClassHint class = { a_name ? a_name : termname, a_class ? a_class : termname };
    XWMHints wm = { .flags = InputHint, .input = 1 };
    XSizeHints *sizeh;

    sizeh = XAllocSizeHints ();

    sizeh->flags = PSize | PResizeInc | PBaseSize | PMinSize;
    sizeh->height = tw.h;
    sizeh->width = tw.w;
    sizeh->height_inc = tw.ch;
    sizeh->width_inc = tw.cw;
    sizeh->base_width = BORDERPX << 1;
    sizeh->base_height = BORDERPY << 1;
    sizeh->min_width = tw.cw + (BORDERPX << 1);
    sizeh->min_height = tw.ch + (BORDERPY << 1);
    
    if ( a_flags & FlagFixedGeometry ) {
        sizeh->flags |= PMaxSize;
        sizeh->min_width = sizeh->max_width = tw.w;
        sizeh->min_height = sizeh->max_height = tw.h;
    }
    if ( xw.gm & (XValue | YValue) ) {
        sizeh->flags |= USPosition | PWinGravity;
        sizeh->x = xw.l;
        sizeh->y = xw.t;
        sizeh->win_gravity = x_geommask_to_gravity (xw.gm);
    }
    XSetWMProperties (xw.dpy, xw.tw, NULL, NULL, NULL, 0, sizeh, &wm, &class);
    XFree (sizeh);
}

int
x_geommask_to_gravity (int mask)
{
    switch ( mask & (XNegative | YNegative) ) {
        case 0:
            return NorthWestGravity;

        case XNegative:
            return NorthEastGravity;

        case YNegative:
            return SouthWestGravity;
    }
    return SouthEastGravity;
}

int
x_font_load (TermFont *f, FcPattern *pattern)
{
    FcPattern *configured;
    FcPattern *match;
    FcResult result;
    XGlyphInfo extents;
    int wantattr, haveattr, ret;

    /* Manually configure instead of calling XftMatchFont
     * so that we can use the configured pattern for
     * "missing glyph" lookups. */
    configured = FcPatternDuplicate (pattern);
    if ( !configured )
        return -1;

    FcConfigSubstitute (NULL, configured, FcMatchPattern);
    XftDefaultSubstitute (xw.dpy, xw.scr, configured);

    match = FcFontMatch (NULL, configured, &result);
    if ( !match ) {
        FcPatternDestroy (configured);
        return -1;
    }
    f->match = XftFontOpenPattern (xw.dpy, match);
    if ( f->match == NULL ) {
        FcPatternDestroy (configured);
        FcPatternDestroy (match);
        return -1;
    }
    /* init font flags */
    ret = 0;

    if ((XftPatternGetInteger (pattern, "slant", 0, &wantattr) == XftResultMatch)) {
        /* Check if xft was unable to find a font with the appropriate
         * slant but gave us one anyway. Try to mitigate. */
        if ( (XftPatternGetInteger (f->match->pattern, "slant", 0, &haveattr) != XftResultMatch) ||
             haveattr < wantattr ) {
            ret |= FontRegularBadSlant;
            fputs ("font slant does not match\n", stderr);
        }
    }
    if ((XftPatternGetInteger(pattern, "weight", 0, &wantattr)
                == XftResultMatch)) {
        if ((XftPatternGetInteger(f->match->pattern, "weight", 0,
                &haveattr) != XftResultMatch) || haveattr != wantattr) {
            ret |= FontRegularBadWeight;
            fputs("font weight does not match\n", stderr);
        }
    }
    XftTextExtentsUtf8(xw.dpy, f->match, (const FcChar8 *)
            ascii_printable, strlen(ascii_printable), &extents);

    f->set = NULL;
    f->pattern = configured;

    f->ascent = f->match->ascent;
    f->descent = f->match->descent;
    f->lbearing = 0;
    f->rbearing = f->match->max_advance_width;

    f->height = f->ascent + f->descent;
    f->width = DIVCEIL(extents.xOff, strlen(ascii_printable));

    return ret;
}

void
x_fonts_load (double fontsize)
{
    static const char msg [] = "can't open font %s";

    FcPattern *pattern;
    double fontval;
    int ret;

    /* clear font flags */
    tw.flags &= ~FONT_MASK;

    /* name */
    if ( *a_font == '-' )
        pattern = XftXlfdParse (a_font, False, False);
    else
        pattern = FcNameParse((FcChar8 *)a_font);

    if (!pattern) {
        error (msg, a_font);
        goto quit;
    }
    /* size */
    if (fontsize > 1) {
        FcPatternDel(pattern, FC_PIXEL_SIZE);
        FcPatternDel(pattern, FC_SIZE);
        FcPatternAddDouble(pattern, FC_PIXEL_SIZE, (double)fontsize);
        dc.usedfontsize = fontsize;
    } else {
        if (FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &fontval) == FcResultMatch)
            dc.usedfontsize = fontval;
        else if (FcPatternGetDouble(pattern, FC_SIZE, 0, &fontval) == FcResultMatch)
            dc.usedfontsize = -1;
        else {
            /* Default font size is 12, if none given. This is to have a known usedfontsize value. */
            FcPatternAddDouble (pattern, FC_PIXEL_SIZE, 12);
            dc.usedfontsize = 12;
        }
        dc.defaultfontsize = dc.usedfontsize;
    }
    /* regular font */
    ret = x_font_load (&dc.rfont, pattern);
    if ( ret == -1 ) {
        error (msg, a_font);
        goto quit;
    }
    /* update term window flags */
    tw.flags |= ret;

    if (dc.usedfontsize < 0) {
        FcPatternGetDouble(dc.rfont.match->pattern, FC_PIXEL_SIZE, 0, &fontval);
        dc.usedfontsize = fontval;
        if (fontsize == 0)
            dc.defaultfontsize = fontval;
    }
    /* Setting character width and height. */
    tw.cw = ceilf(dc.rfont.width * SCALE_CW);
    tw.ch = ceilf(dc.rfont.height * SCALE_CH);

    /* italic */
    FcPatternDel(pattern, FC_SLANT);
    FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ITALIC);

    ret = x_font_load (&dc.ifont, pattern);
    if ( ret == -1 ) {
        error (msg, a_font);
        goto quit;
    }
    /* update term window flags */
    tw.flags |= (ret << 2);

    /* italic/bold */
    FcPatternDel(pattern, FC_WEIGHT);
    FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_BOLD);

    ret = x_font_load (&dc.ibfont, pattern);
    if ( ret == -1 ) {
        error (msg, a_font);
        goto quit;
    }
    /* update term window flags */
    tw.flags |= (ret << 4);

    /* bold */
    FcPatternDel(pattern, FC_SLANT);
    FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ROMAN);

    ret = x_font_load (&dc.bfont, pattern);
    if ( ret == -1 ) {
        error (msg, a_font);
        goto quit;
    }
    /* update term window flags */
    tw.flags |= (ret << 6);

    FcPatternDestroy(pattern);
    return;

quit:
    die ();
    /* NOP */
}

void
x_font_unload (TermFont *f)
{
    if ( f->match != NULL ) {
        XftFontClose (xw.dpy, f->match);
        f->match = NULL;
    }
    if ( f->pattern != NULL ) {
        FcPatternDestroy (f->pattern);
        f->pattern = NULL;
    }
    if ( f->set != NULL ) {
        FcFontSetDestroy (f->set);
        f->set = NULL;
    }
}

void
fontcache_free (void)
{
    Fontcache *fc;

    /* Free the loaded fonts in the font cache.  */
    fc = (Fontcache *) dc.fntcache.items;
    while ( dc.fntcache.nelements != 0 ) {
        XftFontClose(xw.dpy, fc->font);
        fc++;
        dc.fntcache.nelements--;
    }
}

void
x_fonts_unload (void)
{
    fontcache_free ();

    x_font_unload (&dc.rfont);
    x_font_unload (&dc.bfont);
    x_font_unload (&dc.ifont);
    x_font_unload (&dc.ibfont);
}

int
x_im_open (Display *dpy)
{
    XIMCallback imdestroy = { .client_data = NULL, .callback = x_im_destroy };
    XICCallback icdestroy = { .client_data = NULL, .callback = x_ic_destroy };

    xw.ime.xim = XOpenIM(xw.dpy, NULL, NULL, NULL);
    if (xw.ime.xim == NULL)
        return False;

    if (XSetIMValues(xw.ime.xim, XNDestroyCallback, &imdestroy, NULL))
        error ("XSetIMValues:Could not set XNDestroyCallback");

    xw.ime.spotlist = XVaCreateNestedList(0, XNSpotLocation, &xw.ime.spot, NULL);

    if (xw.ime.xic == NULL) {
        xw.ime.xic = XCreateIC(xw.ime.xim, XNInputStyle,
               XIMPreeditNothing | XIMStatusNothing, XNClientWindow,
               xw.tw, XNDestroyCallback, &icdestroy, NULL);
    }
    if (xw.ime.xic == NULL)
        error ("XCreateIC: Could not create input context");

    return True;
}

void
x_im_instantiate (Display *dpy, XPointer client, XPointer call)
{
    if ( x_im_open (dpy) )
        XUnregisterIMInstantiateCallback (xw.dpy, NULL,
               NULL, NULL, x_im_instantiate, NULL);
}

void
x_im_free (void)
{
    XFree(xw.ime.spotlist);
    xw.ime.spotlist = NULL;

    if (xw.ime.xim != NULL) {
        XCloseIM(xw.ime.xim);
        xw.ime.xim = NULL;
    }
}

void
x_im_destroy (XIM xim, XPointer client, XPointer call)
{
    XRegisterIMInstantiateCallback (xw.dpy, NULL, NULL, NULL,
           x_im_instantiate, NULL);
    x_im_free ();
}

void
x_ic_free (void)
{
    if ( xw.ime.xic != NULL ) {
        XDestroyIC (xw.ime.xic);
        xw.ime.xic = NULL;
    }
}

int
x_ic_destroy (XIC xim, XPointer client, XPointer call)
{
    x_ic_free ();
    return True;
}

void
x_free (void)
{
    /* twdow */
    free (xw.specbuf);

    /* selection */
    free (xsel.primary);

    /* color cache */
    x_clrcache_free ();
    thunk_free (&dc.clrcache);

    /* font cache */
    x_fonts_unload ();
    thunk_free (&dc.fntcache);

    x_ic_free ();
    x_im_free ();
   
    if ( xw.cursor != None )
        XFreeCursor (xw.dpy, xw.cursor);

    if ( dc.gc != NULL )
        XFreeGC (xw.dpy, dc.gc);

    if ( xw.buf != None )
        XFreePixmap (xw.dpy, xw.buf);

    if ( xw.draw != NULL )
        XftDrawDestroy (xw.draw);

    if ( xw.tw != None )
        XDestroyWindow (xw.dpy, xw.tw);

    if ( xw.dpy != NULL )
        XCloseDisplay (xw.dpy);
   
    /* tty */
    t_free ();
}

int
x_create (uint cols, uint rows)
{
    XGCValues gcvalues;
    XColor xmousefg, xmousebg;
    Color *bg;

    pid_t thispid = getpid();

    /* display */ 
    xw.dpy = XOpenDisplay (NULL);
    if ( xw.dpy == NULL ) {
        error ("can't open display");
        return EXIT_FAILURE;
    }
    xw.scr = XDefaultScreen(xw.dpy);
    xw.vis = XDefaultVisual(xw.dpy, xw.scr);

    /* font */
    if (!FcInit()) {
        error ("could not init fontconfig");
        return EXIT_FAILURE;
    }
    x_fonts_load (0);

    /* colors */
    xw.cmap = XDefaultColormap(xw.dpy, xw.scr);
    x_colors_load_index ();
   
    /* adjust fixed twdow geometry */
    tw.w = (BORDERPX << 1) + cols * tw.cw;
    tw.h = (BORDERPY << 1) + rows * tw.ch;
    if (xw.gm & XNegative)
        xw.l += DisplayWidth(xw.dpy, xw.scr) - tw.w - 2;
    if (xw.gm & YNegative)
        xw.t += DisplayHeight(xw.dpy, xw.scr) - tw.h - 2;

    /* Events */
    bg = (Color *) dc.clrcache.items + DEFAULT_BG;
    xw.attrs.border_pixel = xw.attrs.background_pixel = bg->pixel;
    
    xw.attrs.bit_gravity = NorthWestGravity;
    xw.attrs.event_mask = FocusChangeMask | KeyPressMask |
       KeyReleaseMask | ExposureMask | VisibilityChangeMask |
       StructureNotifyMask | ButtonMotionMask | ButtonPressMask |
       ButtonReleaseMask;
    xw.attrs.colormap = xw.cmap;

    if ( a_winid == None )
        a_winid = XRootWindow(xw.dpy, xw.scr);
    
    xw.tw = XCreateWindow(xw.dpy, a_winid, xw.l, xw.t, tw.w,
           tw.h, 0, XDefaultDepth(xw.dpy, xw.scr), InputOutput,
           xw.vis, CWBackPixel | CWBorderPixel | CWBitGravity |
           CWEventMask | CWColormap, &xw.attrs);

    memset(&gcvalues, 0, sizeof(gcvalues));
    gcvalues.graphics_exposures = False;
    dc.gc = XCreateGC(xw.dpy, a_winid, GCGraphicsExposures, &gcvalues);
    xw.buf = XCreatePixmap(xw.dpy, xw.tw, tw.w, tw.h, DefaultDepth(xw.dpy, xw.scr));
    XSetForeground(xw.dpy, dc.gc, xw.attrs.border_pixel);
    XFillRectangle(xw.dpy, xw.buf, dc.gc, 0, 0, tw.w, tw.h);

    /* font spec buffer */
    xw.specbuf = x_malloc (cols * sizeof(GlyphFontSpec));

    /* Xft rendering context */
    xw.draw = XftDrawCreate(xw.dpy, xw.buf, xw.vis, xw.cmap);

    /* input methods */
    if ( !x_im_open (xw.dpy) ) {
        XRegisterIMInstantiateCallback(xw.dpy, NULL, NULL,
               NULL, x_im_instantiate, NULL);
    }
    /* white cursor, black outline */
    xw.cursor = XCreateFontCursor(xw.dpy, MOUSE_SHAPE);

    if (XParseColor (xw.dpy, xw.cmap, MOUSE_FG < 16 ?
             basecolornames [MOUSE_FG] : colornames [MOUSE_FG - 256],
             &xmousefg) == 0) {
        xmousefg.red   = 0xffff;
        xmousefg.green = 0xffff;
        xmousefg.blue  = 0xffff;
    }
    if (XParseColor (xw.dpy, xw.cmap, MOUSE_BG < 16 ?
             basecolornames [MOUSE_BG] : colornames [MOUSE_BG - 256],
             &xmousebg) == 0) {
        xmousebg.red   = 0x0000;
        xmousebg.green = 0x0000;
        xmousebg.blue  = 0x0000;
    }
    XRecolorCursor(xw.dpy, xw.cursor, &xmousefg, &xmousebg);
    XDefineCursor(xw.dpy, xw.tw, xw.cursor);

    xw.xembed = XInternAtom(xw.dpy, "_XEMBED", False);
    xw.wmdeletewin = XInternAtom(xw.dpy, "WM_DELETE_WINDOW", False);
    xw.netwmname = XInternAtom(xw.dpy, "_NET_WM_NAME", False);
    xw.netwmiconname = XInternAtom(xw.dpy, "_NET_WM_ICON_NAME", False);
    XSetWMProtocols (xw.dpy, xw.tw, &xw.wmdeletewin, 1);

    xw.netwmpid = XInternAtom(xw.dpy, "_NET_WM_PID", False);
    XChangeProperty(xw.dpy, xw.tw, xw.netwmpid, XA_CARDINAL, 32,
            PropModeReplace, (uchar *)&thispid, 1);

    tw.flags = MODE_NUMLOCK;
    x_set_title (a_title);
    x_hints ();
    XMapWindow (xw.dpy, xw.tw);
    XSync (xw.dpy, False);

    clock_gettime (CLOCK_MONOTONIC, &xsel.tclick1);
    clock_gettime (CLOCK_MONOTONIC, &xsel.tclick2);

    xsel.primary = NULL;
    xsel.clipboard = NULL;
    xsel.xtarget = XInternAtom(xw.dpy, "UTF8_STRING", 0);
    if (xsel.xtarget == None)
        xsel.xtarget = XA_STRING;

    return EXIT_SUCCESS;
}

Fontcache *
fontcache_find (Rune rune, FontcacheFlags flags, FT_UInt *glyphidx)
{
    Fontcache *fc;
    int i;
    FT_UInt idx;
 
    /* Fallback on font cache, search the font cache for match. */
    for ( i = dc.fntcache.nelements, fc = (Fontcache *) dc.fntcache.items;
          i != 0;
          i--, fc++ ) {
        if ( fc->flags != flags )
             continue;
 
        idx = XftCharIndex (xw.dpy, fc->font, rune);
        /* Everything correct. */
        if ( idx != 0 ) {
            *glyphidx = idx;
            return fc;
        }
        /* We got a default font for a not found glyph. */
        if ( fc->unicodep == rune ) {
            *glyphidx = 0;  /* $idx is 0 here */
            return fc;
        }
    }
    return NULL;
}

Fontcache * 
fontcache_add (TermFont *font, Rune rune)
{
    FcPattern *fcpattern, *fontpattern;
    FcFontSet *fcsets;
    FcCharSet *fccharset;
    Fontcache *fc;
    FcResult fcres;
    XftFont *new_font;
 
    /* Nothing was found. Use fontconfig to find matching font. */
    fcsets = font->set;
    if ( fcsets == NULL )
        fcsets = font->set = FcFontSort (0, font->pattern, 1, 0, &fcres);
        /* FIXME: should we check the result ($fcres)? */

    /* Nothing was found in the cache. Now use some dozen of
     * Fontconfig calls to get the font for one single character. */
    fcpattern = FcPatternDuplicate (font->pattern);
    fccharset = FcCharSetCreate ();

    FcCharSetAddChar (fccharset, rune);
    FcPatternAddCharSet (fcpattern, FC_CHARSET, fccharset);
    FcPatternAddBool (fcpattern, FC_SCALABLE, 1);

    FcConfigSubstitute (0, fcpattern, FcMatchPattern);
    FcDefaultSubstitute (fcpattern);

    fontpattern = FcFontSetMatch (0, &fcsets, 1, fcpattern, &fcres);
    new_font = XftFontOpenPattern (xw.dpy, fontpattern);
    
    FcPatternDestroy (fcpattern);
    FcCharSetDestroy (fccharset);
    
    if ( new_font == NULL ) {
        error ("XftFontOpenPattern failed seeking fallback font: %s",
                       strerror(errno));
        die ();
        /* NOP */
    }
    /* Allocate memory for the new cache entry. */
    fc = (Fontcache *) thunk_alloc_next (&dc.fntcache);
    fc->font = new_font;
    fc->unicodep = rune;

    return fc;
}

TermFont *
x_glyph_attr_to_font (GlyphAttribute attr, FontcacheFlags *retflags)
{
    if ( attr & ATTR_BOLD ) {
        if ( attr & ATTR_ITALIC ) {
            /* italic/bold */
            *retflags = FRC_ITALICBOLD;
            return &dc.ibfont;
        }
        /* bold */
        *retflags = FRC_BOLD;
        return &dc.bfont;
    }
   
    if ( attr & ATTR_ITALIC ) {
        /* italic */
        *retflags = FRC_ITALIC;
        return &dc.ifont;
    }
    
    /* regular */
    *retflags = FRC_NORMAL;
    return &dc.rfont;
}

TermFont *
x_glyph_make_font_spec (XftGlyphFontSpec *ps, Rune rune, GlyphAttribute attr, TermFont *font, FontcacheFlags *retflags)
{
    FT_UInt glyphidx;
    Fontcache *fc;
    FontcacheFlags flags;

    /* Determine font for glyph if different from previous glyph. */
    if ( font == NULL )
        font = x_glyph_attr_to_font (attr, retflags);

    /* Lookup character index with default font. */
    glyphidx = XftCharIndex (xw.dpy, font->match, rune);
    if ( glyphidx != 0 )
        ps->font = font->match;
    else {
        /* fetch flags */
        flags = *retflags;

        /* Fallback on font cache, search the font cache for match. */
        fc = fontcache_find (rune, flags, &glyphidx);
        if ( fc == NULL ) {
            /* Nothing was found in the cache; let's add new font for
             * this character */
            fc = fontcache_add (font, rune);
            fc->flags = flags;
            glyphidx = XftCharIndex (xw.dpy, fc->font, rune);
        }
        ps->font = fc->font;
    }

    /* add new entry */
    ps->glyph = glyphidx;
    return font;
}

int
x_glyph_make_font_specs (XftGlyphFontSpec *specs, const TermGlyph *glyphs, int len, int col, int row)
{
    GlyphAttribute attr;
    int numspecs;
    TermFont *font, *prevfont;
    FontcacheFlags flags;
    float yp, runewidth;
    
    float xp = BORDERPX + col * tw.cw;
    float twy = BORDERPY + row * tw.ch;
 
    for ( numspecs = 0, prevfont = NULL;
          len != 0;
          len--, glyphs++ ) {
        /* Skip dummy wide-character spacing. */
        attr = glyphs->attr;
        if ( attr == ATTR_WDUMMY )
            continue;

        /* Determine font for glyph if different from previous glyph. */
        font = x_glyph_make_font_spec (specs, glyphs->rune, attr, prevfont, &flags);
        
        if ( prevfont != font ) {
            prevfont = font;
            yp = twy + font->ascent;
           
            runewidth = tw.cw;
            if ( attr & ATTR_WIDE )
                runewidth *= 2;
        }
        specs->x = (short) xp;
        specs->y = (short) yp;

        xp += runewidth;
        specs++;
        numspecs++;
    }

    return numspecs;
}

void
x_glyph_draw_font_specs (const XftGlyphFontSpec *specs, uint len,
       uint col, uint row, GlyphAttribute attr, uint fg, uint bg)
{
    Color *clrfg, *clrbg;
    XRectangle r;
    int winx, winy, width;
 
    /* Fallback on color display for attributes not supported by the font */
    if ( attr & ATTR_BOLD ) {
        if ( attr & ATTR_ITALIC ) {
            /* bold/italic */
            if (twin_flag (FontBoldItalicBadWeight | FontBoldItalicBadSlant) )
                fg = DEFAULT_ATTR;
        } else {
            /* bold */
            if ( twin_flag (FontBoldBadWeight) )
                fg = DEFAULT_ATTR;
        }
    } else if ( attr & ATTR_ITALIC ) {
        /* italic */
        if ( twin_flag (FontItalicBadSlant) )
            fg = DEFAULT_ATTR;
    }

    /* faint? */
    width = attr & ATTR_BOLD_FAINT;  /* $width is used as a temp */
    if ( width == ATTR_BOLD ) {
        if ( fg < 8 )
            /* Change basic system colors [0-7] to bright system colors [8-15] */
            fg += 8;
    } else if ( width == ATTR_FAINT ) {
        /* create new color */
        width = x_color_load_faint (fg);
        if ( width != -1 )
            fg = width;
    }

    /* reverse single glyph? */
    if ( attr & ATTR_REVERSE ) {
        width = fg;
        fg = bg;
        bg = width;
    }
    if ( (attr & ATTR_BLINK && twin_flag (MODE_BLINK)) ||
          attr & ATTR_INVISIBLE )
        fg = bg;

    /* fetch colors from cache */
    clrfg = (Color *) dc.clrcache.items + fg;
    clrbg = (Color *) dc.clrcache.items + bg;

    /* Intelligent cleaning up of the borders. */
    winx = BORDERPX + col * tw.cw;
    winy = BORDERPY + row * tw.ch;

    width = tw.cw * len;
    if ( attr & ATTR_WIDE )
        width <<= 1;

    r.y = winy + tw.ch >= BORDERPY + tw.th;  /* variable is used as a temp only */
 
    if (col == 0)
        x_clear (0,
                 row == 0 ? 0 : winy,
                 BORDERPX,
                 winy + tw.ch + (r.y ? tw.h : 0));

    if ( winx + width >= BORDERPX + tw.tw )
        x_clear (winx + width,
                 row == 0 ? 0 : winy,
                 tw.w,
                 r.y ? tw.h : winy + tw.ch);

    if ( row == 0 )
        x_clear (winx,
                 0,
                 winx + width,
                 BORDERPY);

    if ( r.y )
        x_clear (winx,
                 winy + tw.ch,
                 winx + width,
                 tw.h);

    /* Clean up the region we want to draw to. */
    XftDrawRect (xw.draw, clrbg, winx, winy, width, tw.ch);

    /* Set the clip region because Xft is sometimes dirty. */
    r.x = 0;
    r.y = 0;
    r.height = tw.ch;
    r.width = width;

    XftDrawSetClipRectangles (xw.draw, winx, winy, &r, 1);

    /* Render the glyphs. */
    XftDrawGlyphFontSpec (xw.draw, clrfg, specs, len);

    /* Render underline and strikethrough. */
    if (attr & ATTR_UNDERLINE)
        XftDrawRect (xw.draw, clrfg, winx, winy + dc.rfont.ascent + 1, width, 1);

    if (attr & ATTR_STRUCK)
        XftDrawRect (xw.draw, clrfg, winx, winy + (dc.rfont.ascent << 1) / 3, width, 1);

    /* Reset clip to none. */
    XftDrawSetClip (xw.draw, 0);
}

void
x_glyph_draw (Rune rune, uint col, uint row, GlyphAttribute attr, uint fg, uint bg)
{
    XftGlyphFontSpec spec;
    FontcacheFlags flags;
    TermFont *font;

    font = x_glyph_make_font_spec (&spec, rune, attr, NULL, &flags);
    spec.x = BORDERPX + col * tw.cw;
    spec.y = BORDERPY + row * tw.ch + font->ascent;

    x_glyph_draw_font_specs (&spec, 1, col, row, attr, fg, bg);
}

void
x_cursor_draw_inactive (Color *drawcol, uint col, uint row)
{
    XftDrawRect (xw.draw, drawcol,
        BORDERPX + col * tw.cw,
        BORDERPY + row * tw.ch,
        tw.cw - 1, 1);

    XftDrawRect (xw.draw, drawcol,
        BORDERPX + col * tw.cw,
        BORDERPY + row * tw.ch,
        1, tw.ch - 1);

    XftDrawRect (xw.draw, drawcol,
        BORDERPX + (col + 1) * tw.cw - 1,
        BORDERPY + row * tw.ch,
        1, tw.ch - 1);

    XftDrawRect (xw.draw, drawcol,
        BORDERPX + col * tw.cw,
        BORDERPY + (row + 1) * tw.ch - 1,
        tw.cw, 1);
}

int
x_cursor_draw_non_glyph (Color *drawcol, uint col, uint row)
{
    switch ( tw.cursor ) {
        case 3:  /* Blinking Underline */
        case 4:  /* Steady Underline */
            XftDrawRect (xw.draw, drawcol,
                BORDERPX + col * tw.cw,
                BORDERPY + (row + 1) * tw.ch - CURSOR_THICKNESS,
                tw.cw, CURSOR_THICKNESS);
            return True;

        case 5:  /* Blinking bar */
        case 6:  /* Steady bar */
            XftDrawRect (xw.draw, drawcol,
                BORDERPX + col * tw.cw,
                BORDERPY + row * tw.ch,
                CURSOR_THICKNESS, tw.ch);
            return True;
    }
    return False;
}

void
x_cursor_remove (TermGlyph *tg, uint col, uint row)
{
    GlyphAttribute attr;

    /* fetch */
    attr = tg->attr;

    /* remove the old cursor */
    if ( t_selected (col, row) )
        attr ^= ATTR_REVERSE;

    x_glyph_draw (tg->rune, col, row, attr, tg->fg, tg->bg);
}
 
void
x_cursor_draw (Rune rune, GlyphAttribute attr, uint col, uint row)
{
    Color* drawcol;
    uint fg, bg;

    /* hidden cursor? */
    if ( twin_flag (MODE_HIDE) )
        return;

    /* Select the right color for the right mode. */
    drawcol = (Color *) dc.clrcache.items;
    fg = t_selected (col, row);  /* $fg_g used as a temp */

    if ( twin_flag (MODE_REVERSE))
        drawcol += fg ? DEFAULT_CS : DEFAULT_RCS;
    else
        drawcol += fg ? DEFAULT_RCS : DEFAULT_CS;

    /* inactive window? */
    if ( !twin_flag (MODE_FOCUSED) ) {
        x_cursor_draw_inactive (drawcol, col, row);
        return;
    }

    /* non glyph cursor? */
    if ( x_cursor_draw_non_glyph (drawcol, col, row) )
        return;

    attr &= ATTR_BOLD | ATTR_ITALIC | ATTR_UNDERLINE | ATTR_STRUCK | ATTR_WIDE;

    if ( twin_flag (MODE_REVERSE)) {
        attr |= ATTR_REVERSE;
        bg = DEFAULT_FG;
        fg = fg ? DEFAULT_RCS : DEFAULT_CS;
    } else {
        if ( fg ) {
            fg = DEFAULT_FG;
            bg = DEFAULT_RCS;
        } else {
            fg = DEFAULT_BG;
            bg = DEFAULT_CS;
        }
    }
    
    switch (tw.cursor) {
        case 7: /* st extension */
            rune = 0x2603; /* snowman (U+2603) */
            /* FALLTHROUGH */

        case 0: /* Blinking Block */
        case 1: /* Blinking Block (Default) */
        case 2: /* Steady Block */
            x_glyph_draw (rune, col, row, attr, fg, bg);
            break;
    }
}

void
x_set_env (void)
{
    char buf [sizeof(long) * 8 + 1];

    snprintf (buf, sizeof (buf), "%lu", xw.tw);
    setenv ("WINDOWID", buf, 1);
}

#ifdef FEATURE_TITLE
char *
x_get_title_atom (Atom atom)
{
    XTextProperty prop;
    char **strs;
    int count;
    char *ret = NULL;

    XGetTextProperty (xw.dpy, xw.tw, &prop, atom);
    if ( XmbTextPropertyToTextList (xw.dpy, &prop, &strs,
               &count) == Success && count != 0 ) {
        ret = s_dup (*strs);
        XFreeStringList (strs);
    }
    XFree(prop.value);
    return ret;
}

char *
x_get_icon_title (void)
{
    return x_get_title_atom (xw.netwmiconname);
}

char *
x_get_title (void)
{
    return x_get_title_atom (xw.netwmname);
}

int
x_set_title_atom (const char *p, XTextProperty *prop, Atom atom)
{
    if ( Xutf8TextListToTextProperty (xw.dpy, (char **) &p, 1, XUTF8StringStyle, prop) != Success )
        return 1;

    XSetTextProperty (xw.dpy, xw.tw, prop, atom);
    return 0;
}

int
x_set_icon_title (const char *p)
{
    XTextProperty prop;

    if ( x_set_title_atom (p, &prop, xw.netwmiconname ) != 0 )
        return 1;

    XSetWMIconName (xw.dpy, xw.tw, &prop);
    XFree(prop.value);
    return 0;
}

int
x_set_title (const char *p)
{
    XTextProperty prop;

    if ( x_set_title_atom (p, &prop, xw.netwmname ) != 0 )
        return 1;

    XSetWMName(xw.dpy, xw.tw, &prop);
    XFree(prop.value);
    return 0;
}
#endif  /* FEATURE_TITLE */

int
x_is_mode_visible (void)
{
    return twin_flag (MODE_VISIBLE);
}

void
x_line_draw (Line line, uint row, uint col1, uint col2, uint sel)
{
    uint numspecs, cntspecs, base_col, cmin, cmax;
    uint cur_fg, cur_bg, base_fg, base_bg;
    XftGlyphFontSpec *specs;
    GlyphAttribute cur_attr, base_attr;
   
    specs = xw.specbuf;
    line += col1;
    numspecs = x_glyph_make_font_specs (specs, line, col2 - col1, col1, row);

    /* selection */
    if ( sel )
        sel = tline_sel_get_margin (row, &cmin, &cmax);
 
    /* find base values */
    for ( ; col1 < col2; col1++, line++ ) {
        /* fetch mode */
        base_attr = line->attr;
        if ( base_attr == ATTR_WDUMMY )
            continue;

        /* selected? */
        if ( sel && BETWEEN (col1, cmin, cmax) )
            base_attr ^= ATTR_REVERSE;

        /* fetch mode, fg and bg */
        base_fg = line->fg;
        base_bg = line->bg;

        base_col = col1;
        cntspecs = 1;
        goto process;
    }

    /* we could not find base values therefore quit */
    return;

    /* main loop */
process:
    for ( ;; ) {
        if ( ++col1 == col2 )
            break;

        /* fetch mode */
        line++;
        cur_attr = line->attr;
        if ( cur_attr == ATTR_WDUMMY )
            continue;

        /* selected? */
        if ( sel && BETWEEN (col1, cmin, cmax) )
            cur_attr ^= ATTR_REVERSE;

        /* fetch mode, fg and bg */
        cur_fg = line->fg;
        cur_bg = line->bg;

        if ( cur_attr == base_attr &&
             cur_fg == base_fg &&
             cur_bg == base_bg ) {
            cntspecs++;
            continue;
        }

        /* draw glyphs with same style */
        x_glyph_draw_font_specs (specs, cntspecs, base_col, row,
                                base_attr, base_fg, base_bg);

        /* update glyph buffer */
        specs += cntspecs;
        numspecs -= cntspecs;
        cntspecs = 1;

        base_attr = cur_attr;
        base_fg = cur_fg;
        base_bg = cur_bg;
        base_col = col1;
    }

    /* draw remaining glyphs */
    x_glyph_draw_font_specs (specs, cntspecs, base_col, row,
                             base_attr, base_fg, base_bg);
}

void
x_draw_finish (void)
{
    Color *c;

    /* we don't need to handle MODE_REVERSE because the colors are swapped in
     * this mode */
    c = (Color *) dc.clrcache.items + DEFAULT_BG;

    XCopyArea (xw.dpy, xw.buf, xw.tw, dc.gc, 0, 0, tw.w, tw.h, 0, 0);
    XSetForeground (xw.dpy, dc.gc, c->pixel);
}

void
x_im_spot(int col, int row)
{
    if (xw.ime.xic == NULL)
        return;

    xw.ime.spot.x = BORDERPX + col * tw.cw;
    xw.ime.spot.y = BORDERPY + (row + 1) * tw.ch;

    XSetICValues(xw.ime.xic, XNPreeditAttributes, xw.ime.spotlist, NULL);
}

void
expose (XEvent *ev)
{
    t_draw (True);
}

void
visibility (XEvent *ev)
{
    XVisibilityEvent *e = &ev->xvisibility;

    MODBIT (tw.flags, e->state != VisibilityFullyObscured, MODE_VISIBLE);
}

void
unmap (XEvent *ev)
{
    tw.flags &= ~MODE_VISIBLE;
}

void
x_set_pointer_motion (int set)
{
    MODBIT (xw.attrs.event_mask, set, PointerMotionMask);
    XChangeWindowAttributes (xw.dpy, xw.tw, CWEventMask, &xw.attrs);

    if ( set )
        XUndefineCursor (xw.dpy, xw.tw);
    else
        XDefineCursor (xw.dpy, xw.tw, xw.cursor);
}

void
x_set_mode (int set, uint flags)
{
    TermWindowFlags oldflags;
   
    oldflags = tw.flags;
    MODBIT (tw.flags, set, flags);

    if ( twin_flag (MODE_REVERSE) != (oldflags & MODE_REVERSE) ) {
        /* re-create all true colors and redraw */
        x_colors_reverse ();
        t_draw (True);
    }
}

int
x_set_cursor (int cursor)
{
    if (!BETWEEN(cursor, 0, 7))  /* 7: st extension */
        return 1;
    
    tw.cursor = cursor;
    return 0;
}

void
x_set_urgency (int add)
{
    XWMHints *h = XGetWMHints(xw.dpy, xw.tw);

    MODBIT(h->flags, add, XUrgencyHint);
    XSetWMHints(xw.dpy, xw.tw, h);
    XFree(h);
}

void
x_bell (void)
{
    if ( !(twin_flag (MODE_FOCUSED)) )
        x_set_urgency (True);
    if ( BELL_VOLUME )
        XkbBell (xw.dpy, xw.tw, BELL_VOLUME, (Atom)NULL);
}

void
focus (XEvent *ev)
{
    XFocusChangeEvent *e = &ev->xfocus;

    if (e->mode == NotifyGrab)
        return;

    if (ev->type == FocusIn) {
        if (xw.ime.xic)
            XSetICFocus(xw.ime.xic);

        tw.flags |= MODE_FOCUSED;
        x_set_urgency(False);

        if (twin_flag(MODE_FOCUS))
            tty_write ("\033[I", 3, 0);
    } else {
        if (xw.ime.xic)
            XUnsetICFocus(xw.ime.xic);

        tw.flags &= ~MODE_FOCUSED;
        if (twin_flag(MODE_FOCUS))
            tty_write ("\033[O", 3, 0);
    }
}

int
match (uint mask, uint state)
{
    return mask == XK_ANY_MOD || mask == (state & ~IGNORE_MOD);
}

char*
kmap (KeySym k, uint state)
{
    Key *kp;
    int i;

    /* Check for mapped keys out of X11 function keys. */
    for (i = 0; i < LEN(mappedkeys); i++) {
        if (mappedkeys[i] == k)
            break;
    }
    if (i == LEN(mappedkeys)) {
        if ((k & 0xFFFF) < 0xFD00)
            return NULL;
    }
    for (kp = key; kp < key + LEN(key); kp++) {
        if (kp->k != k)
            continue;

        if (!match(kp->mask, state))
            continue;

        if (twin_flag(MODE_APPKEYPAD) ? kp->appkey < 0 : kp->appkey > 0)
            continue;

        if (twin_flag(MODE_NUMLOCK) && kp->appkey == 2)
            continue;

        if (twin_flag(MODE_APPCURSOR) ? kp->appcursor < 0 : kp->appcursor > 0)
            continue;

        return kp->s;
    }
    return NULL;
}

void
kpress (XEvent *ev)
{
    XKeyEvent *e = &ev->xkey;
    KeySym ksym;
    char buf[64], *customkey;
    int len;
    Rune c;
    Status status;
    Shortcut *bp;

    if (twin_flag(MODE_KBDLOCK))
        return;

    if (xw.ime.xic)
        len = XmbLookupString(xw.ime.xic, e, buf, sizeof buf, &ksym, &status);
    else
        len = XLookupString(e, buf, sizeof buf, &ksym, NULL);

    /* 1. shortcuts */
    for (bp = shortcuts; bp < shortcuts + LEN(shortcuts); bp++) {
        if (ksym == bp->keysym && match(bp->mod, e->state)) {
            bp->func(&(bp->arg));
            return;
        }
    }
    /* 2. custom keys from config.h */
    if ((customkey = kmap(ksym, e->state))) {
        tty_write (customkey, strlen(customkey), True);
        return;
    }
    /* 3. composed string from input method */
    if (len == 0)
        return;
    if (len == 1 && e->state & Mod1Mask) {
        if (twin_flag(MODE_8BIT)) {
            if (*buf < 0177) {
                c = *buf | 0x80;
                len = utf8_encode (c, buf);
            }
        } else {
            buf[1] = buf[0];
            buf[0] = '\033';
            len = 2;
        }
    }
    tty_write (buf, len, True);
}

void
cmessage (XEvent *e)
{
    /* See xembed specs http://standards.freedesktop.org/xembed-spec/xembed-spec-latest.html */
    if (e->xclient.message_type == xw.xembed && e->xclient.format == 32) {
        if (e->xclient.data.l[1] == XEMBED_FOCUS_IN) {
            tw.flags |= MODE_FOCUSED;
            x_set_urgency (False);
        } else if (e->xclient.data.l[1] == XEMBED_FOCUS_OUT)
            tw.flags &= ~MODE_FOCUSED;
    } else if (e->xclient.data.l[0] == xw.wmdeletewin) {
        tty_hangup();
        x_exit ();
        /* NOP */
    }
}

void
resize (XEvent *e)
{
    if ( e->xconfigure.width  == tw.w &&
         e->xconfigure.height == tw.h )
        return;

    cresize (e->xconfigure.width, e->xconfigure.height);
}

void
run (const char **argv, uint argn)
{
    XEvent ev;
    fd_set rfd;
    int w, h, xfd, ttyfd, xev, dratwg, ttypending;
    struct timespec seltv, *tv, now, lastblink, trigger;
    double timeout;
    EventHandler eh;

    /* init */
    w = tw.w;
    h = tw.h;
    xfd = XConnectionNumber(xw.dpy);

    /* Waiting for twdow mapping */
    do {
        XNextEvent(xw.dpy, &ev);

        /* This XFilterEvent call is required because of XOpenIM. It does
         * filter out the key event and some client message for the input
         * method too. */
        if (XFilterEvent(&ev, None))
            continue;
        if (ev.type == ConfigureNotify) {
            w = ev.xconfigure.width;
            h = ev.xconfigure.height;
        }
    } while (ev.type != MapNotify);

    ttyfd = tty_new (argv, argn);
    cresize (w, h);

    for (timeout = -1, dratwg = False, lastblink = (struct timespec){0};;) {
        FD_ZERO(&rfd);
        FD_SET(ttyfd, &rfd);
        FD_SET(xfd, &rfd);

        if (XPending(xw.dpy)
#ifdef FEATURE_SYNC_UPDATE                
            || tty_read_pending()
#endif  
           ) timeout = 0;  /* existing events might not set xfd */

        seltv.tv_sec = timeout / 1E3;
        seltv.tv_nsec = 1E6 * (timeout - 1E3 * seltv.tv_sec);
        tv = timeout >= 0 ? &seltv : NULL;

        if (pselect(MAX(xfd, ttyfd) + 1, &rfd, NULL, NULL, tv, NULL) < 0) {
            if (errno == EINTR)
                continue;
            error ("select failed: %s", strerror(errno));
            die ();
            /* NOP */
        }
        clock_gettime(CLOCK_MONOTONIC, &now);

        ttypending = FD_ISSET(ttyfd, &rfd)
#ifdef FEATURE_SYNC_UPDATE                
            || tty_read_pending()
#endif
        ;        
        if (ttypending)
            tty_read ();

        xev = False;
        while (XPending(xw.dpy)) {
            xev = True;
            XNextEvent(xw.dpy, &ev);

            if (XFilterEvent(&ev, None))
                continue;

            /* fetch handler and execute if not null */
            eh = events [ev.type];
            if ( eh != NULL )
                eh (&ev);
        }
        /* To reduce flicker and tearing, when new content or event
         * triggers dratwg, we first wait a bit to ensure we got
         * everything, and if nothing new arrives - we draw.
         * We start with trying to wait LATENCY_MIN ms. If more content
         * arrives sooner, we retry with shorter and shorter periods,
         * and eventually draw even without idle after LATENCY_MAX ms.
         * Typically this results in low latency while interacting,
         * maximum latency intervals during `cat huge.txt`, and perfect
         * sync with periodic updates from animations/key-repeats/etc. */
        if (ttypending || xev) {
            if (!dratwg) {
                trigger = now;
                dratwg = True;
            }
            timeout = (LATENCY_MAX - TIMEDIFF(now, trigger)) / LATENCY_MAX * LATENCY_MIN;
            if (timeout > 0)
                continue;  /* we have time, try to find idle */
        }
        
#ifdef FEATURE_SYNC_UPDATE
        /* sync update mode */
        if ( tty_sync_update () && tsu_clock () ) {
            /* on synchronized-update draw-suspension: don't reset
             * dratwg so that we draw ASAP once we can (just after ESU).
             * it won't be too soon because we already can draw now but
             * we skip. we set timeout > 0 to draw on SU-timeout even
             * without new content. */
             timeout = LATENCY_MIN;
             continue;
        }
#endif  /* FEATURE_SYNC_UPDATE */

        /* idle detected or LATENCY_MAX exhausted -> draw */
        timeout = -1;
        if (BLINK_TIMEOUT && t_attr_set (ATTR_BLINK)) {
            timeout = BLINK_TIMEOUT - TIMEDIFF(now, lastblink);
            if (timeout <= 0) {
                if (-timeout > BLINK_TIMEOUT) /* start visible */
                    tw.flags |= MODE_BLINK;
                tw.flags ^= MODE_BLINK;
                t_attr_set_dirt (ATTR_BLINK);
                lastblink = now;
                timeout = BLINK_TIMEOUT ;
            }
        }
        t_draw (False);
        XFlush (xw.dpy);
        dratwg = False;
    }
}

void
usage (void)
{
    /* TODO: add the description, see --version, --raw etc. */
    static const char *help_message =
        "<options> include:\n"
        "    --version | -V             print program version\n"
        "    --raw | -r                 raw output\n"
        "    --verbose | -v\n"
#ifdef FEATURE_TITLE
        "    --title=<title>\n"
#endif        
        "    --class=<class>\n"
        "    --font=<font> | -f <font>\n"
        "    --geo=<geometry>           window geometry <cols>x<rows>{+-}<left>{+-}<top>\n"
        "    --name=<name>\n"
        "    --io=<path>                empty <path>...\n"
        "    --line=<path>              use -- or - for stty commands\n"
        "    --altscr | -a              allow alt screen\n"
        "    --fixgeo | -x              fix geometry\n"
        "    --id={0x<id> | <id>}       embed\n"
        "    -- | -                     command list";

    verbose_help ();
    verbose_color (stdout, "usage", VerboseWhite);
    verbose_s (" [--<options>] [-ahrvx]");
    verbose_newline ();

    verbose_s (help_message);
    verbose_newline ();
}

void
version (void)
{
    verbose_color (stdout, VERSION, VerboseWhite);
    verbose_newline ();
}

const char *
get_prog_name (const char *name)
{
    char *p;
   
    p = strrchr (name, '/');
    if ( p != NULL )
        return p + 1;

    return name;
}

int
main(int argc, char *argv[])
{
    uint cols, rows;

    /* program name */
    prog_name = get_prog_name (*argv);

    /* arguments */
    argc = args_parse (++argv, --argc);
    if ( argc == -1 )
        return EXIT_FAILURE;

    /* usage */
    if ( a_flags & ( FlagHelp | FlagVersion ) ) {
        if ( a_flags & FlagHelp )
            usage ();
        if ( a_flags & FlagVersion )
            version ();
        return EXIT_SUCCESS;
    }
#ifdef FEATURE_TITLE
    /* title */
    if ( a_title == NULL )
        a_title = a_line != NULL || argc == 0 ?
            TITLE : *argv;
#endif  /* FEATURE_TITLE */
 
    /* parse geometry */
    cols = COLUMNS;
    rows = ROWS;

    xw.gm = XParseGeometry (a_geo, &xw.l, &xw.t, &cols, &rows);
    cols = MAX (cols, 1);
    rows = MAX (rows, 1);
   
    /* cursor */
    x_set_cursor (CURSOR_SHAPE);

    if ( !setlocale (LC_CTYPE, "") || !XSupportsLocale () )
        warn ("no locale support");

    XSetLocaleModifiers ("");
   
    /* init thunks */
    t_init ();
    thunk_create (&dc.clrcache, MAX_INDEX_CACHE, sizeof (Color));
    thunk_create (&dc.fntcache, 0, sizeof (Fontcache));

    /* create and run */
    t_new (cols, rows);
    x_create (cols, rows);
    
    x_set_env ();
    run ((const char **) argv, argc);

    x_free();
    return EXIT_SUCCESS;
}
