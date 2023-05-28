/* See LICENSE file for copyright and license details. */

#ifndef _ARGS_H_
#define _ARGS_H_

#include <X11/Xlib.h>
#include "st.h"


typedef enum {
    FlagVersion        = (1 << 0),
    FlagHelp           = (1 << 1),
    FlagRaw            = (1 << 2),  /* print uncolorized info */
    FlagAllowAltScreen = (1 << 3),  /* alt screens */
    FlagFixedGeometry  = (1 << 4),  /* is fixed geometry? */
    FlagVerbose        = (1 << 5)
} ArgsFlags;


extern ArgsFlags a_flags;
extern char *a_line;
extern char *a_class;
extern char *a_font;
extern char *a_io;
extern char *a_name;
extern char *a_geo;
extern Window a_winid;
#ifdef FEATURE_TITLE
extern char *a_title;
#endif  /* FEATURE_TITLE */


extern const char version_arg[];
extern const char help_arg[];
extern const char verbose_arg[];
extern const char altscr_arg[];
extern const char class_arg[];
extern const char font_arg[];
extern const char geo_arg[];
extern const char fixgeo_arg[];
extern const char io_arg[];
extern const char line_arg[];
extern const char name_arg[];
extern const char id_arg[];
#ifdef FEATURE_TITLE
extern const char title_arg[];
#endif  /* FEATURE_TITLE */


/*
 * Args
 */

typedef struct {
    int count;
    char **v;
    char *arg;
    char *name;
    char *value;
} Args;

typedef enum {
    ArgsEnd,
    ArgsDouble,
    ArgsSingle,
    ArgsWord
} ArgsStatus;


int args_copy_remaining (char **dstv, char **srcv, uint srcn);
ArgsStatus args_fetch_next (Args *args, const char *argname, int no_parse );
int args_parse (char **argv, uint argn);


#endif  /* _ARGS_H_ */
