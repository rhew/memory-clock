#ifndef MOM_CLOCK_APP_H
#define MOM_CLOCK_APP_H

#include "clock_config.h"

void clock_app_start(const clock_config_t *config);
const clock_config_t *clock_app_config(void);

#endif
