/* See LICENSE file for copyright and license details. */

#include <string.h>
#include <errno.h>

#include "strutil.h"
#include "verbose.h"
#include "st.h"


char *
s_set_zero (char *s)
{
    *s++ = '0';
    *s = '\0';
    return s;
}

/* e points to s + strlen (s) ~ *e should be '\0' */
void
s_reverse_end (char *s, char *e)
{
    char c;

    while ( s < --e ) {
        /* swap characters */
        c = *s;
        *s++ = *e;
        *e = c;
    }
}

void
s_reverse (char *s)
{
    unsigned int len;

    len = strlen (s);
    s_reverse_end (s, s + len);
}

char *
s_uint (char *s, unsigned int val)
{
    char *e;

    if ( val == 0 )
        return s_set_zero (s);

    /* process individual digits */
    e = s;
    while ( val != 0 ) {
        *e++ = '0' + val % 10;
        val /= 10;
    }
    s_reverse_end (s, e);

    *e = '\0';
    return e;
}

char *
s_int (char *s, int val)
{
    if ( val < 0 ) {
        *s++ = '-';
        val = -val;
    }
    return s_uint (s, val);
}

char *
s_dup (const char *s)
{
    s = strdup (s);
    if ( s == NULL ) {
        error (msg_out_of_memory, strerror(errno));
        die ();
        /* NOP */
    }
    return (char *) s;
}

char *
s_hex (char *s, unsigned int val)
{
    int mod;
    char *e;

    *s++ = '0';
    *s++ = 'x';

    if ( val == 0 )
        return s_set_zero (s);

    /* process individual digits */
    e = s;
    while ( val != 0 ) {
        mod = val % 16;
        *e++ = mod < 10 ? '0' + mod : '1' + mod;  /* ~ 'A' + mod - 10 */
        val /= 16;
    }
    s_reverse_end (s, e);

    *e = '\0';
    return e;
}
