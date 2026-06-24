#include "clock_app.h"

#include "ui.h"

static clock_config_t app_config;

void clock_app_start(const clock_config_t *config)
{
    app_config = *config;
    ui_create();
}

const clock_config_t *clock_app_config(void)
{
    return &app_config;
}
