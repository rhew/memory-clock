#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "banner.h"
#include "display_port.h"
#include "provisioning.h"

static const char *TAG = "memory_clock";
static uint8_t banner_buffer[BANNER_BUFFER_SIZE];

enum {
    BUTTON_RIGHT_GPIO = 4,
    BUTTON_LEFT_GPIO = 5,
    BUTTON_DEBOUNCE_MS = 250,
    FULL_REFRESH_MINUTE_INTERVAL = 10,
    LOOP_POLL_MS = 100,
    SNTP_WAIT_MS = 15000,
};

typedef enum {
    PAGE_NAV_NONE = 0,
    PAGE_NAV_PREVIOUS = -1,
    PAGE_NAV_NEXT = 1,
} page_nav_t;

static void timezone_init(void)
{
    setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1);
    tzset();
}

static esp_err_t clock_sync_time(void)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("time.cloudflare.com");
    ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&config), TAG, "sntp init");
    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(SNTP_WAIT_MS));
    if(err != ESP_OK) {
        esp_netif_sntp_deinit();
        return err;
    }
    return ESP_OK;
}

static const char *clock_daypart(int hour)
{
    if(hour < 5) return "Night";
    if(hour < 12) return "Morning";
    if(hour < 17) return "Afternoon";
    if(hour < 21) return "Evening";
    return "Night";
}

static void render_page_frame(size_t page_index, struct tm *tm_info,
                              banner_clock_layout_t *layout)
{
    char weekday[16];
    char month_text[16];
    char date_text[32];
    int hour12 = tm_info->tm_hour % 12;
    if(hour12 == 0) hour12 = 12;
    strftime(weekday, sizeof(weekday), "%A", tm_info);
    strftime(month_text, sizeof(month_text), "%B", tm_info);
    snprintf(date_text, sizeof(date_text), "%s %d, %d", month_text, tm_info->tm_mday,
             tm_info->tm_year + 1900);
    banner_render_page(banner_buffer, sizeof(banner_buffer), page_index, weekday,
                       clock_daypart(tm_info->tm_hour), hour12, tm_info->tm_min,
                       tm_info->tm_hour >= 12, date_text, layout);
}

static esp_err_t buttons_init(void)
{
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << BUTTON_LEFT_GPIO) | (1ULL << BUTTON_RIGHT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    return gpio_config(&config);
}

static page_nav_t buttons_poll(void)
{
    static int last_left = 1;
    static int last_right = 1;
    static int64_t last_left_event_us;
    static int64_t last_right_event_us;

    int64_t now_us = esp_timer_get_time();
    int left = gpio_get_level(BUTTON_LEFT_GPIO);
    int right = gpio_get_level(BUTTON_RIGHT_GPIO);
    page_nav_t nav = PAGE_NAV_NONE;

    if(left == 0 && last_left != 0
       && (now_us - last_left_event_us) / 1000 >= BUTTON_DEBOUNCE_MS) {
        last_left_event_us = now_us;
        nav = PAGE_NAV_PREVIOUS;
    } else if(right == 0 && last_right != 0
              && (now_us - last_right_event_us) / 1000 >= BUTTON_DEBOUNCE_MS) {
        last_right_event_us = now_us;
        nav = PAGE_NAV_NEXT;
    }

    last_left = left;
    last_right = right;
    return nav;
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if(err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    timezone_init();

    banner_render_status(banner_buffer, sizeof(banner_buffer), "Connecting to",
                         provisioning_ssid());
    ESP_ERROR_CHECK(display_port_show_monochrome_full(banner_buffer, sizeof(banner_buffer),
                                                      BANNER_WIDTH, BANNER_HEIGHT));

    char ip_address[16];
    err = provisioning_start(ip_address, sizeof(ip_address));
    if(err == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi connected: %s -> %s", provisioning_ssid(), ip_address);
    } else {
        banner_render_status(banner_buffer, sizeof(banner_buffer), "Failed",
                             provisioning_ssid());
        ESP_LOGE(TAG, "Wi-Fi connection failed for %s: %s",
                 provisioning_ssid(), esp_err_to_name(err));
        ESP_ERROR_CHECK(display_port_show_monochrome_full(banner_buffer, sizeof(banner_buffer),
                                                          BANNER_WIDTH, BANNER_HEIGHT));
        return;
    }

    banner_render_status(banner_buffer, sizeof(banner_buffer), "Syncing time", "time.cloudflare.com");
    ESP_ERROR_CHECK(display_port_show_monochrome_full(banner_buffer, sizeof(banner_buffer),
                                                      BANNER_WIDTH, BANNER_HEIGHT));
    ESP_ERROR_CHECK(clock_sync_time());
    ESP_LOGI(TAG, "time synchronized from %s", "time.cloudflare.com");
    ESP_ERROR_CHECK(buttons_init());

    int last_minute = -1;
    size_t page_count = banner_page_count();
    size_t current_page = 0;
    bool force_full_refresh = true;
    banner_clock_layout_t clock_layout = {0};
    ESP_LOGI(TAG, "configured %u display page(s)", (unsigned)page_count);
    while(true) {
        time_t now = time(NULL);
        struct tm tm_info;
        localtime_r(&now, &tm_info);

        bool minute_changed = tm_info.tm_min != last_minute;
        bool should_render = force_full_refresh || (current_page == 0 && minute_changed);
        if(should_render) {
            render_page_frame(current_page, &tm_info, &clock_layout);
            if(force_full_refresh || current_page != 0 || last_minute < 0
               || (tm_info.tm_min % FULL_REFRESH_MINUTE_INTERVAL) == 0) {
                ESP_ERROR_CHECK(display_port_show_monochrome_full(banner_buffer,
                                                                  sizeof(banner_buffer),
                                                                  BANNER_WIDTH, BANNER_HEIGHT));
            } else {
                ESP_ERROR_CHECK(display_port_show_monochrome_partial(banner_buffer,
                                                                     sizeof(banner_buffer),
                                                                     BANNER_WIDTH, BANNER_HEIGHT,
                                                                     clock_layout.minute_x,
                                                                     clock_layout.minute_y,
                                                                     clock_layout.minute_width,
                                                                     clock_layout.minute_height));
            }
            force_full_refresh = false;
            if(current_page == 0) {
                last_minute = tm_info.tm_min;
            }

            char timestamp[32];
            strftime(timestamp, sizeof(timestamp), "%I:%M %p", &tm_info);
            ESP_LOGI(TAG, "displayed page %u/%u at %s",
                     (unsigned)(current_page + 1), (unsigned)page_count, timestamp);
        }

        page_nav_t nav = buttons_poll();
        if(nav != PAGE_NAV_NONE && page_count > 1) {
            if(nav == PAGE_NAV_NEXT) {
                current_page = (current_page + 1) % page_count;
            } else {
                current_page = (current_page + page_count - 1) % page_count;
            }
            force_full_refresh = true;
            ESP_LOGI(TAG, "page button selected page %u/%u",
                     (unsigned)(current_page + 1), (unsigned)page_count);
        } else if(nav != PAGE_NAV_NONE) {
            ESP_LOGI(TAG, "page button ignored because only one page is configured");
        } else {
            vTaskDelay(pdMS_TO_TICKS(LOOP_POLL_MS));
        }
    }
}
