/* See LICENSE file for copyright and license details. */

#ifndef _VERBOSE_H_
#define _VERBOSE_H_

#include <stdarg.h>
#include <stdio.h>


#define verbose_b(b)       (verbose_boolean (stdout, b))
#define verbose_s(s)       (fputs ((s), stdout))
#define verbose_c(c)       (fputc ((c), stdout))
#define verbose_newline()  (verbose_c ('\n'))
#define verbose_comma()    { verbose_c (','); verbose_c (' '); }

#define error_b(b)         (verbose_boolean (stderr, b))
#define error_s(s)         (fputs ((s), stderr))
#define error_c(c)         (fputc ((c), stderr))
#define error_newline()    (verbose_c ('\n'))
#define error_comma()      { verbose_c (','); verbose_c (' '); }


typedef enum {
    VerboseUndefined,
    VerboseBlack   = 30,
    VerboseRed     = 31,
    VerboseGreen   = 32,
    VerboseYellow  = 33,
    VerboseBlue    = 34,
    VerboseMagenda = 35,
    VerboseCyan    = 36,
    VerboseWhite   = 37
} VerboseColor;


extern const char *prog_name;

extern const char msg_arg_unknown [];
extern const char msg_arg_unknown_char [];
extern const char msg_arg_missing [];
extern const char msg_invalid_winid [];
extern const char msg_out_of_memory [];
extern const char msg_csi_arg_missing [];


extern const char help_name[];
extern const char warning_name[];
extern const char error_name[];
extern const char info_name[];


void verbose_color_begin (FILE *file, VerboseColor color);
void verbose_color_end (FILE *file);
void verbose_color (FILE *file, const char *str, VerboseColor color);
void verbose_prefix (FILE *file, const char *prefix);
void verbose_color_prefix (FILE *file, const char *prefix, VerboseColor color);
void verbose (FILE *file, const char *prefix, VerboseColor color, const char *err, va_list params);

void verbose_boolean (FILE *file, int val);

void verbose_help (void);
void verbose_error (void);
void verbose_warn (void);
void verbose_info (void);

void help (const char *format, ...);
void info (const char *format, ...);
void warn (const char *format, ...);
void error (const char *format, ...);


#endif /* _VERBOSE_H_ */
