/* See LICENSE file for copyright and license details. */

#ifndef _THUNK_H_
#define _THUNK_H_

#include "st.h"


#define thunk_index_to_bytes(t, i)  ((t)->element_size * (i))
#define thunk_to_bytes(t)           ((t)->element_size * (t)->nelements)

#define thunk_get_item(t, i)        ((t)->items + thunk_index_to_bytes (t, i))
#define thunk_get_end(t)            ((t)->items + thunk_to_bytes (t))
#define thunk_free(t)               (free ((t)->items))


typedef struct {
    byte *items;        /* elements */
    uint alloc_size;    /* allocated elements */
    uint element_size;  /* sizeof (<Element>) */
    uint nelements;     /* elements used */
} Thunk;


void thunk_init (Thunk *thunk);
void thunk_create (Thunk *thunk, uint init_size, uint element_size);
byte * thunk_double_size (Thunk *thunk, uint min_size);
byte * thunk_alloc_next (Thunk *thunk);


#endif  /* _THUNK_H_ */
