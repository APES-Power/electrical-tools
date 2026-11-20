#ifndef _CABINET__H_
#define _CABINET__H_

#include <stdint.h>

#include "apes_types.h"
#include "load.h"

typedef struct {
    bool_t      initialized;
    bool_t      active;

    uint32_t    id;
    uint32_t    revision;
    string_t    tag;
    string_t    name;

    cabinet_t  *cabinet_array;
    load_t     *load_array;

} cabinet_t;

#endif