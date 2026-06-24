#ifndef MOM_CLOCK_SIM_CONFIG_LOADER_H
#define MOM_CLOCK_SIM_CONFIG_LOADER_H

#include "clock_config.h"

#include <stdbool.h>
#include <stddef.h>

bool sim_config_load(const char *path, clock_config_t *config, char *error, size_t error_size);

#endif
