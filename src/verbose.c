/* See LICENSE file for copyright and license details. */

#include "verbose.h"
#include "args.h"


const char *prog_name;

const char msg_arg_unknown []      = "unrecognized argument: %s";
const char msg_arg_unknown_char [] = "unrecognized argument: -%c";
const char msg_arg_missing []      = "%s requires an argument";
const char msg_invalid_winid []    = "window: invalid id # %s";
const char msg_out_of_memory []    = "out of memory: %s";
const char msg_csi_arg_missing []  = "CSI(%d): missing arguments";

const char help_name[]    = "HELP";
const char warning_name[] = "WARNING";
const char error_name[]   = "ERROR";
const char info_name[]    = "INFO";


void
verbose_color_begin (FILE *file, VerboseColor color)
{
    if ( (a_flags & FlagRaw) == 0 )
        fprintf (file, "\033[%d;1m", color);
}

void
verbose_color_end (FILE *file)
{
    if ( (a_flags & FlagRaw) == 0 )
        fputs ("\033[0m", file);
}

void verbose_color (FILE *file, const char *str, VerboseColor color)
{
    verbose_color_begin (file, color);
    fputs (str, file);
    verbose_color_end (file);
}

void
verbose_prefix (FILE *file, const char *prefix)
{
    /* "$prefix: " */
    fputs (prefix, file);
    fputc (':', file);
    fputc (' ', file);
}

void
verbose_color_prefix (FILE *file, const char *prefix, VerboseColor color)
{
    verbose_color_begin (file, color);
    verbose_prefix (file, prefix);
    verbose_color_end (file);
}

void
verbose_help (void)
{
    verbose_prefix (stdout, prog_name);
    verbose_color_prefix (stdout, help_name, VerboseMagenda);
}

void
help (const char *format, ...)
{
    va_list params;

    verbose_help ();
    va_start (params, format);
    vfprintf (stdout, format, params);
    va_end (params);
    verbose_newline ();
}
 
void
verbose_warn (void)
{
    verbose_prefix (stderr, prog_name);
    verbose_color_prefix (stderr, warning_name, VerboseYellow);
}

void
warn (const char *format, ...)
{
    va_list params;

    verbose_warn ();
    va_start (params, format);
    vfprintf (stderr, format, params);
    va_end (params);
    verbose_newline ();
}

void
verbose_error (void)
{
    verbose_prefix (stderr, prog_name);
    verbose_color_prefix (stderr, error_name, VerboseRed);
}

void
error (const char *format, ...)
{
    va_list params;

    verbose_error ();
    va_start (params, format);
    vfprintf (stderr, format, params);
    va_end (params);
    verbose_newline ();
}

void
verbose_info (void)
{
    verbose_prefix (stdout, prog_name);
    verbose_color_prefix (stdout, info_name, VerboseGreen);
}

void
info (const char *format, ...)
{
    va_list params;

    verbose_info ();
    va_start (params, format);
    vfprintf (stdout, format, params);
    va_end (params);
    verbose_newline ();
}

void
verbose_boolean (FILE *file, int val)
{
    if ( val )
        verbose_color (file, "yes", VerboseGreen);
    else
        verbose_color (file, "no", VerboseRed);
}
