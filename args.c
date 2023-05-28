/* See LICENSE file for copyright and license details. */

#include <string.h>
#include "args.h"
#include "verbose.h"

const char version_arg [] = "version";
const char help_arg [] = "help";
const char verbose_arg [] = "verbose";
const char altscr_arg [] = "altscr";
const char class_arg [] = "class";
const char font_arg [] = "font";
const char geo_arg [] = "geo";
const char fixgeo_arg [] = "fixgeo";
const char io_arg [] = "io";
const char line_arg [] = "line";
const char name_arg [] = "name";
const char id_arg [] = "id";
const char raw_arg [] = "raw";
#ifdef FEATURE_TITLE
const char title_arg [] = "title";
#endif /* FEATURE_TITLE */

ArgsFlags a_flags = FlagAllowAltScreen;
char *a_line = NULL;
char *a_class = NULL;
char *a_font = FONT;
char *a_io = NULL;
char *a_name = NULL;
char *a_geo = NULL;
#ifdef FEATURE_TITLE
char *a_title = NULL;
#endif /* FEATURE_TITLE */
Window a_winid = None;

/*
 * Args
 */

int args_copy_remaining(char **dstv, char **srcv, uint srcn)
{
    uint count = 0;

    /* copy remaining arguments */
    while (srcn-- != 0)
    {
        *dstv++ = *srcv++;
        count++;
    }
    *dstv = NULL;
    return count;
}

ArgsStatus
args_fetch_next(Args *args, const char *argname, int no_parse)
{
    char *name;
    char *value;
    ArgsStatus stat;

    /* no arguments? */
    if (args->count-- == 0)
    {
        if (argname != NULL)
            error(msg_arg_missing, argname);
        return ArgsEnd;
    }

    /* get next argument */
    name = args->arg = *args->v++;
    if (no_parse)
        return ArgsWord;

    /* check '--' and '-' */
    if (*name == '-')
    {
        if (*++name == '-')
        {
            /* parse name/value pair */
            value = strchr(++name, '=');
            if (value == NULL)
                args->value = NULL;
            else
            {
                *value++ = '\0';
                args->value = value;
            }
            stat = ArgsDouble;
        }
        else
            stat = ArgsSingle;
    }
    else
        stat = ArgsWord;

    args->name = name;
    return stat;
}

/*
 * Run params
 */

int args_parse(char **argv, uint argn)
{
    Args cur;
    ArgsStatus stat;
    char c;
    Window w;

    /* init */
    cur.count = argn;
    cur.v = argv;

    /* handle arguments */
    for (;;)
    {
        stat = args_fetch_next(&cur, NULL, False);
        if (stat == ArgsEnd)
            break;

        /* --abcde arguments */
        if (stat == ArgsDouble)
        {
            /* -- */
            if (*cur.name == '\0' && cur.value == NULL)
                /* copy remaining arguments */
                return args_copy_remaining(argv, cur.v, cur.count);

            /* --version */
            if (strcmp(cur.name, version_arg) == 0)
            {
                a_flags |= FlagVersion;
                continue;
            }
            /* --help */
            if (strcmp(cur.name, help_arg) == 0)
            {
                a_flags |= FlagHelp;
                continue;
            }
            /* --verbose */
            if (strcmp(cur.name, verbose_arg) == 0)
            {
                a_flags |= FlagVerbose;
                continue;
            }
            /* --altscr */
            if (strcmp(cur.name, altscr_arg) == 0)
            {
                a_flags |= FlagAllowAltScreen;
                continue;
            }
            /* --fixgeo */
            if (strcmp(cur.name, fixgeo_arg) == 0)
            {
                a_flags |= FlagFixedGeometry;
                continue;
            }
            /* --raw */
            if (strcmp(cur.name, raw_arg) == 0)
            {
                a_flags |= FlagRaw;
                continue;
            }
            /* --class */
            if (strcmp(cur.name, class_arg) == 0)
            {
                if (cur.value == NULL)
                {
                    error(msg_arg_missing, cur.arg);
                    return -1;
                }
                a_class = cur.value;
                continue;
            }
            /* --font */
            if (strcmp(cur.name, font_arg) == 0)
            {
                if (cur.value == NULL)
                {
                    error(msg_arg_missing, cur.arg);
                    return -1;
                }
                a_font = cur.value;
                continue;
            }
            /* --geo */
            if (strcmp(cur.name, geo_arg) == 0)
            {
                if (cur.value == NULL)
                {
                    error(msg_arg_missing, cur.arg);
                    return -1;
                }
                a_geo = cur.value;
                continue;
            }
            /* --io */
            if (strcmp(cur.name, io_arg) == 0)
            {
                if (cur.value == NULL)
                {
                    error(msg_arg_missing, cur.arg);
                    return -1;
                }
                a_io = cur.value;
                continue;
            }
            /* --line */
            if (strcmp(cur.name, line_arg) == 0)
            {
                if (cur.value == NULL)
                {
                    error(msg_arg_missing, cur.arg);
                    return -1;
                }
                a_line = cur.value;
                continue;
            }
            /* --name */
            if (strcmp(cur.name, name_arg) == 0)
            {
                if (cur.value == NULL)
                {
                    error(msg_arg_missing, cur.arg);
                    return -1;
                }
                a_name = cur.value;
                continue;
            }
#ifdef FEATURE_TITLE
            /* --title */
            if (strcmp(cur.name, title_arg) == 0)
            {
                if (cur.value == NULL)
                {
                    error(msg_arg_missing, cur.arg);
                    return -1;
                }
                a_title = cur.value;
                continue;
            }
#endif /* FEATURE_TITLE */
            /* --id */
            if (strcmp(cur.name, id_arg) == 0)
            {
                if (cur.value == NULL)
                {
                    error(msg_arg_missing, cur.arg);
                    return -1;
                }

                sscanf(cur.value, "0x%lx", &w);
                if (w == None)
                    sscanf(cur.value, "%lu", &w);
                if (w == None)
                {
                    error(msg_invalid_winid, cur.value);
                    return -1;
                }
                a_winid = w;
                continue;
            }
            /* unknown argument */
            error(msg_arg_unknown, cur.arg);
            return -1;
        }

        /* -$ arguments */
        if (stat == ArgsSingle)
        {
            if (*cur.name == '\0')
                /* copy remaining arguments */
                return args_copy_remaining(argv, cur.v, cur.count);

            /* -f ~ --font */
            if (strcmp(cur.name, "f") == 0)
            {
                if (args_fetch_next(&cur, font_arg, True) == ArgsEnd)
                    return -1;
                a_font = cur.arg;
                continue;
            }
            for (c = *cur.name; c != '\0'; c = *++cur.name)
            {
#if 0                
                /* -e: this implementation allows to use arguments
                 * with no parameters together e.g. -aIe <cmds> */
                if ( c == 'e' ) {
                    /* copy remaining arguments */
                    args_copy_remaining (&copy, &cur);
                    break;
                }
#endif
                /* -a ~ --altscreen */
                if (c == 'a')
                    a_flags |= FlagAllowAltScreen;
                /* -i ~ --fixed */
                else if (c == 'i')
                    a_flags |= FlagFixedGeometry;
                /* -h ~ --help */
                else if (c == 'h')
                    a_flags |= FlagHelp;
                /* -r ~ --help */
                else if (c == 'r')
                    a_flags |= FlagRaw;
                /* -v ~ --verbose */
                if (c == 'v')
                    a_flags |= FlagVerbose;
                /* -V ~- --version */
                else if (c == 'V')
                    a_flags |= FlagVersion;
                /* unknown argument */
                else
                {
                    error(msg_arg_unknown_char, c);
                    return -1;
                }
            }
        }
    }
    return 0;
}
