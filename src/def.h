/* See LICENSE file for copyright and license details. */

#ifndef _DEF_H_
#define _DEF_H_

/* Default columns and rows numbers  */
#define COLUMNS  /*19*/60
#define ROWS     /*5*/16

/* title */
#define FEATURE_TITLE
#define TITLE  "STerm"

/* font: see http://freedesktop.org/software/fontconfig/fontconfig-user.html */
#define FONT  "Nimbus Mono L:pixelsize=16:antialias=true:autohint=true"

/* What program is execed by st depends of these precedence rules:
 * 1: program passed with --
 * 2: scroll and/or utmp (see bellow)
 * 3: SHELL environment variable
 * 4: value of shell in /etc/passwd
 */
#define SHELL  "/opt/sh/bin/zsh"

/* scroll program: to enable uncomment and use a string like "scroll" */
//#define SCROLL  "scroll"
//#define UTMP    "utmp"

#define STTY_ARGS  "stty raw pass8 nl -echo -iexten -cstopb 38400"

/*
 * appearance
 */
#define BORDERPX  2
#define BORDERPY  2


/* Kerning / character bounding-box multipliers */
#define SCALE_CW  1
#define SCALE_CH  1

/* selection timeouts (in milliseconds) */
#define CLICK_DOUBLE_TIMEOUT   300
#define CLICK_TRIPPLE_TIMEOUT  600

/* allow certain non-interactive (insecure) window operations such as:
   setting the clipboard text */
//#define ALLOW_WINDOW_OPS

/*
 * draw latency range in ms - from new content/keypress/etc until drawing.
 * within this range, st draws when content stops arriving (idle). mostly it's
 * near minlatency, but it waits longer for slow updates to avoid partial draw.
 * low minlatency will tear/flicker more, as it can "detect" idle too early.
 */
#define LATENCY_MIN  8
#define LATENCY_MAX  33

/*
 * Synchronized-Update timeout in ms
 * http://gitlab.com.peihua.vpn358.com:8082/gnachman/iterm2/-/wikis/synchronized-updates-spec
 */
#define SYNC_TIMEOUT  200

/*
 * blinking timeout (set to 0 to disable blinking) for the terminal blinking
 * attribute.
 */
#define BLINK_TIMEOUT  0

/*
 * thickness of underline and bar cursors
 */
#define CURSOR_THICKNESS  2

/*
 * bell volume. It must be a value between -100 and 100. Use 0 for disabling
 * it
 */
#define BELL_VOLUME  0

/*
 * spaces per tab
 *
 * When you are changing this value, don't forget to adapt the »it« value in
 * the st.info and appropriately install the st.info in the environment where
 * you use this st version.
 *
 *	it#$tabspaces,
 *
 * Secondly make sure your kernel is not expanding tabs. When running `stty
 * -a` »tab0« should appear. You can tell the terminal to not expand tabs by
 *  running following command:
 *
 *	stty tabs
 */
#define TAB_SPACES  8

/*
 * Default colors (colorname index)
 * foreground, background, cursor, reverse cursor
 */
#define DEFAULT_FG   256
#define DEFAULT_BG   0
#define DEFAULT_CS   256
#define DEFAULT_RCS  256

/*
 * Default shape of cursor
 * 2: Block ("█")
 * 4: Underline ("_")
 * 6: Bar ("|")
 * 7: Snowman ("☃")
 */
#define CURSOR_SHAPE  4

/*
 * Default colour and shape of the mouse cursor
 */
#define MOUSE_SHAPE  XC_xterm

/* both values must be < 16 or > 255 */
#define MOUSE_FG     256
#define MOUSE_BG     0

/*
 * Color used to display font attributes when fontconfig selected a font which
 * doesn't match the ones requested.
 */
#define DEFAULT_ATTR  11

/*
 * Force mouse select/shortcuts while mask is active (when MODE_MOUSE is set).
 * Note that if you want to use ShiftMask with selmasks, set this to an other
 * modifier, set to 0 to not use it.
 */
#define FORCE_MOUSE_MOD  ShiftMask

/* Internal keyboard shortcuts. */
#define MODKEY    Mod1Mask
#define TERMMODE  (ControlMask | ShiftMask)

/*
 * State bits to ignore when matching key or button events.  By default,
 * numlock (Mod2Mask) and keyboard layout (XK_SWITCH_MOD) are ignored.
 */
#define IGNORE_MOD  (Mod2Mask | XK_SWITCH_MOD)

/*
 * Selection types' masks.
 * Use the same masks as usual.
 * Button1Mask is always unset, to make masks match between ButtonPress.
 * ButtonRelease and MotionNotify.
 * If no match is found, regular selection is used.
 */
#define SEL_RECTANGULAR_MASK  Mod1Mask


typedef unsigned char  byte;
typedef unsigned char  uchar;
typedef unsigned int   uint;
typedef unsigned long  ulong;
typedef unsigned short ushort;


#endif  /* _DEF_H_ */
