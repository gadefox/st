/* See LICENSE file for copyright and license details. */

#ifndef _STRING_H_
#define _STRING_H_

#define MAX_HEX_SIZE  (64 / 8 + 3)  /* 64bit / 8 + strlen("0x\0") */


char * s_dup (const char *s);

char * s_set_zero (char *s);
void s_reverse_end (char *s, char *e);
void s_reverse (char *s);

char * s_uint (char *s, unsigned int val);
char * s_int (char *s, int val);
char * s_hex (char *s, unsigned int val);


#endif  /* _STRING_H_ */
