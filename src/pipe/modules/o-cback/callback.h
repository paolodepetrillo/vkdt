#pragma once

#include "pipe/token.h"

#include <stdint.h>

typedef void (*dt_output_callback_t)(void *param, dt_token_t inst, int wd, int ht, const uint8_t *data);

typedef struct {
    dt_output_callback_t callback;
    void *param;
} dt_ocback_data_t;
