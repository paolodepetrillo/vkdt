#pragma once

#include "token.h"

#include <stdint.h>

typedef void (*dt_output_callback_t)(dt_token_t inst, int wd, int ht, const uint8_t *data);

extern dt_output_callback_t dt_output_callback;
