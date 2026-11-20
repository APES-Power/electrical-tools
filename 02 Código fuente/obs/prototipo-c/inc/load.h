#ifndef _LOAD__H_
#define _LOAD__H_

#include <stdint.h>

#include "apes_types.h"
#include "load_type.h"

typedef struct {
    bool_t      initialized;
    bool_t      active;

    uint32_t    id;
    uint32_t    revision;
    string_t    tag;
    string_t    name;
    load_type_t type;
    uint8_t     num_phases;
    bool_t      neutral;
    double      voltage;
    double      frequency;
    double      nominal_power;
    double      power_factor;
    double      efficiency;
    double      use_factor;
    double      simultaneity_factor;
    double      emergency_factor;
    double      demanded_active_power;
    double      demanded_reactive_power;
    double      demanded_apparent_power;
} load_t;

apes_status_t load_init(load_t *load)
{
    load->initialized = true;
    load->active = 0;

    load->id = 0;
    load->revision = 0;
    load->tag = NULL;
    load->name = NULL;
    load->type = NULL;
    load->num_phases = 0;
    load->neutral = 0;
    load->voltage = 0.0f;
    load->frequency = 0.0f;
    load->nominal_power = 0.0f;
    load->power_factor = 0.0f;
    load->efficiency = 0.0f;
    load->use_factor = 0.0f;
    load->simultaneity_factor = 0.0f;
    load->emergency_factor = 0.0f;
    load->demanded_active_power = 0.0f;
    load->demanded_reactive_power = 0.0f;
    load->demanded_apparent_power = 0.0f;

    return APES_OK;
}

#endif