#define _POSIX_C_SOURCE 200809L
#define SDL_MAIN_HANDLED

#include <SDL2/SDL.h>
#include <lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "clock_app.h"
#include "config_loader.h"

enum {
    DISPLAY_WIDTH = 800,
    DISPLAY_HEIGHT = 480,
};

int main(int argc, char **argv)
{
    const char *config_path = argc > 1 ? argv[1] : "sample/config.json";
    clock_config_t config;
    char error[256];

    if(!sim_config_load(config_path, &config, error, sizeof(error))) {
        fprintf(stderr, "Config error: %s\n", error);
        return 1;
    }

    if(setenv("TZ", config.timezone, 1) != 0) {
        fprintf(stderr, "Config error: cannot set timezone\n");
        return 1;
    }
    tzset();

    lv_init();
    lv_tick_set_cb(SDL_GetTicks);

    lv_display_t *display = lv_sdl_window_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_sdl_window_set_title(display, "Mom Clock Simulator");

    clock_app_start(&config);

    for(;;) {
        lv_timer_handler();
        SDL_Delay(5);
    }
}
