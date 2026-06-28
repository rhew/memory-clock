#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_check.h"
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
    FULL_REFRESH_MINUTE_INTERVAL = 10,
    SNTP_WAIT_MS = 15000,
};

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

static void render_clock_frame(struct tm *tm_info, banner_clock_layout_t *layout)
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
    banner_render_clock(banner_buffer, sizeof(banner_buffer), weekday,
                        clock_daypart(tm_info->tm_hour), hour12, tm_info->tm_min,
                        tm_info->tm_hour >= 12, date_text, layout);
}

static void sleep_until_next_minute(void)
{
    time_t now = time(NULL);
    int delay_seconds = 60 - (int)(now % 60);
    if(delay_seconds <= 0 || delay_seconds > 60) {
        delay_seconds = 60;
    }
    vTaskDelay(pdMS_TO_TICKS(delay_seconds * 1000));
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

    int last_minute = -1;
    banner_clock_layout_t clock_layout = {0};
    while(true) {
        time_t now = time(NULL);
        struct tm tm_info;
        localtime_r(&now, &tm_info);
        if(tm_info.tm_min == last_minute) {
            sleep_until_next_minute();
            continue;
        }

        render_clock_frame(&tm_info, &clock_layout);
        if(last_minute < 0 || (tm_info.tm_min % FULL_REFRESH_MINUTE_INTERVAL) == 0) {
            ESP_ERROR_CHECK(display_port_show_monochrome_full(banner_buffer, sizeof(banner_buffer),
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

        char timestamp[32];
        strftime(timestamp, sizeof(timestamp), "%I:%M %p", &tm_info);
        ESP_LOGI(TAG, "displayed time %s", timestamp);
        last_minute = tm_info.tm_min;
        sleep_until_next_minute();
    }
}
