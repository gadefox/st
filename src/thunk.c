/* See LICENSE file for copyright and license details. */

#include <stdlib.h>
#include "thunk.h"


#define THUNK_INIT_SIZE  8


void
thunk_init (Thunk *thunk)
{
    thunk->items = NULL;
    thunk->nelements = 0;
}

void
thunk_create (Thunk *thunk, uint init_size, uint element_size)
{
    if ( init_size == 0 )
        init_size = THUNK_INIT_SIZE;

    thunk->items = x_malloc (init_size * element_size);
    thunk->alloc_size = init_size;
    thunk->element_size = element_size;
    thunk->nelements = 0;
}

byte *
thunk_double_size (Thunk *thunk, uint min_size)
{
    byte *new_items;
    uint double_size;

    /* double buffer size */
    double_size = thunk->alloc_size << 1;
    /* min_size can be 0: then the condition is ignored (double_size > 0) */
    if ( double_size < min_size )
        double_size = min_size;
    
    new_items = x_realloc (thunk->items,
           thunk_index_to_bytes (thunk, double_size));

    /* update the buffer */
    thunk->items = new_items;
    thunk->alloc_size = double_size;

    return new_items;
}

byte *
thunk_alloc_next (Thunk *thunk)
{
    byte *items;
    uint nelements;

    nelements = thunk->nelements;
    if ( nelements < thunk->alloc_size )
        /* $items [nelements] */
        items = thunk_get_item (thunk, nelements);
    else {
        /* reallocate the buffer */
        items = thunk_double_size (thunk, 0);
        items += thunk_index_to_bytes (thunk, nelements);
    }
    /* increment # and return next item */
    thunk->nelements++;
    return items;
}
