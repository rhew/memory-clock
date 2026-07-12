#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "banner.h"
#include "clock_client.h"
#include "display_port.h"
#include "image_store.h"
#include "provisioning.h"
#include "status.h"
#include "wifi_env.h"

static const char *TAG = "memory_clock";
static uint8_t banner_buffer[BANNER_BUFFER_SIZE];

enum {
    BUTTON_HOME_GPIO = 3,
    BUTTON_RIGHT_GPIO = 4,
    BUTTON_LEFT_GPIO = 5,
    BUTTON_DEBOUNCE_MS = 120,
    FULL_REFRESH_MINUTE_INTERVAL = 10,
    LOOP_POLL_MS = 100,
    POLL_TASK_STACK_BYTES = 8192,
    POLL_TASK_PRIORITY = 1,
};

#define CLOCK_POLL_INTERVAL_TICKS pdMS_TO_TICKS(MEMORY_CLOCK_POLL_INTERVAL_MS)

typedef enum {
    PAGE_NAV_PREVIOUS = -1,
    PAGE_NAV_NEXT = 1,
} page_nav_t;

static portMUX_TYPE button_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile int pending_page_delta;
static volatile bool pending_home_page;
static volatile TickType_t last_button_press_tick[3];
static volatile bool button_down[3];
static portMUX_TYPE server_update_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool pending_server_redraw;
static volatile bool pending_server_reset_page;

static void button_isr_handler(void *arg);
static void server_poll_task(void *arg);

static void timezone_init(void)
{
    setenv("TZ", MEMORY_CLOCK_TIME_ZONE, 1);
    tzset();
}

static esp_err_t clock_sync_time(void)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(MEMORY_CLOCK_TIME_SERVER);
    ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&config), TAG, "sntp init");
    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(MEMORY_CLOCK_SNTP_SYNC_TIMEOUT_MS));
    if(err != ESP_OK) {
        esp_netif_sntp_deinit();
        return err;
    }
    return ESP_OK;
}

static const char *clock_daypart(int hour)
{
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
                       tm_info->tm_hour >= 12, date_text, status_flags(), layout);
}

static esp_err_t buttons_init(void)
{
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << BUTTON_HOME_GPIO) | (1ULL << BUTTON_LEFT_GPIO)
                        | (1ULL << BUTTON_RIGHT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "button gpio config");

    esp_err_t err = gpio_install_isr_service(0);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(BUTTON_LEFT_GPIO, button_isr_handler, (void *)1),
                        TAG, "left button handler");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(BUTTON_RIGHT_GPIO, button_isr_handler, (void *)2),
                        TAG, "right button handler");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(BUTTON_HOME_GPIO, button_isr_handler, (void *)0),
                        TAG, "home button handler");
    return ESP_OK;
}

static void button_isr_handler(void *arg)
{
    int button = (int)(intptr_t)arg;
    int gpio = button == 0 ? BUTTON_HOME_GPIO
               : button == 1 ? BUTTON_LEFT_GPIO
                             : BUTTON_RIGHT_GPIO;
    TickType_t now = xTaskGetTickCountFromISR();
    TickType_t debounce_ticks = pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS);
    bool pressed = gpio_get_level(gpio) == 0;

    if(pressed) {
        if(button_down[button]) return;
        if(now - last_button_press_tick[button] < debounce_ticks) return;
        button_down[button] = true;
        last_button_press_tick[button] = now;
        portENTER_CRITICAL_ISR(&button_lock);
        if(button == 0) {
            pending_home_page = true;
        } else {
            pending_page_delta += button == 1 ? PAGE_NAV_PREVIOUS : PAGE_NAV_NEXT;
        }
        portEXIT_CRITICAL_ISR(&button_lock);
    } else {
        if(!button_down[button]) return;
        button_down[button] = false;
    }
}

static int buttons_take_pending_delta(void)
{
    portENTER_CRITICAL(&button_lock);
    int delta = pending_page_delta;
    pending_page_delta = 0;
    portEXIT_CRITICAL(&button_lock);
    return delta;
}

static bool buttons_take_pending_home(void)
{
    portENTER_CRITICAL(&button_lock);
    bool home = pending_home_page;
    pending_home_page = false;
    portEXIT_CRITICAL(&button_lock);
    return home;
}

static size_t page_advance(size_t page, size_t page_count, int delta)
{
    if(page_count <= 1 || delta == 0) return page;

    int wrapped_delta = delta % (int)page_count;
    if(wrapped_delta < 0) wrapped_delta += (int)page_count;
    return (page + (size_t)wrapped_delta) % page_count;
}

static bool apply_pending_navigation(size_t *current_page, size_t page_count)
{
    if(buttons_take_pending_home()) {
        *current_page = 0;
        ESP_LOGI(TAG, "home button selected page 1/%u", (unsigned)page_count);
        return true;
    }

    int delta = buttons_take_pending_delta();
    if(delta == 0) return false;
    if(page_count <= 1) {
        ESP_LOGI(TAG, "page buttons ignored because only one page is configured");
        return false;
    }

    size_t next_page = page_advance(*current_page, page_count, delta);
    if(next_page == *current_page) {
        ESP_LOGI(TAG, "page buttons netted no page change");
        return false;
    }

    *current_page = next_page;
    ESP_LOGI(TAG, "page buttons selected page %u/%u",
             (unsigned)(*current_page + 1), (unsigned)page_count);
    return true;
}

static void publish_server_update(bool reset_page)
{
    portENTER_CRITICAL(&server_update_lock);
    pending_server_redraw = true;
    pending_server_reset_page = pending_server_reset_page || reset_page;
    portEXIT_CRITICAL(&server_update_lock);
}

static bool take_server_update(bool *reset_page)
{
    portENTER_CRITICAL(&server_update_lock);
    bool redraw = pending_server_redraw;
    if(reset_page != NULL) {
        *reset_page = pending_server_reset_page;
    }
    pending_server_redraw = false;
    pending_server_reset_page = false;
    portEXIT_CRITICAL(&server_update_lock);
    return redraw;
}

static void server_poll_task(void *arg)
{
    (void)arg;
    while(true) {
        image_store_status_t previous_status = image_store_status();
        size_t previous_count = image_store_count();
        clock_client_result_t result = clock_client_poll();
        status_set_server_reachable(result != CLOCK_CLIENT_ERROR);
        status_sample_battery();
        image_store_status_t current_status = image_store_status();
        size_t current_count = image_store_count();

        if(result == CLOCK_CLIENT_UPDATED) {
            publish_server_update(true);
            ESP_LOGI(TAG, "server pages updated; queued redraw of first page");
        } else if(result == CLOCK_CLIENT_ERROR
                  && (current_status != previous_status || current_count != previous_count)) {
            publish_server_update(current_count == 0);
        }

        vTaskDelay(CLOCK_POLL_INTERVAL_TICKS);
    }
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
    ESP_ERROR_CHECK(image_store_init());
    ESP_ERROR_CHECK(status_init());
    status_sample_battery();
    ESP_LOGI(TAG, "firmware version %s", MEMORY_CLOCK_VERSION);

    banner_render_status(banner_buffer, sizeof(banner_buffer), "Connecting to",
                         provisioning_ssid());
    ESP_ERROR_CHECK(display_port_show_monochrome_full(banner_buffer, sizeof(banner_buffer),
                                                      BANNER_WIDTH, BANNER_HEIGHT));

    char ip_address[16];
    err = provisioning_start(ip_address, sizeof(ip_address));
    if(err == ESP_OK) {
        status_set_wifi_connected(true);
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

    banner_render_status(banner_buffer, sizeof(banner_buffer), "Syncing time",
                         MEMORY_CLOCK_TIME_SERVER);
    ESP_ERROR_CHECK(display_port_show_monochrome_full(banner_buffer, sizeof(banner_buffer),
                                                      BANNER_WIDTH, BANNER_HEIGHT));
    ESP_ERROR_CHECK(clock_sync_time());
    ESP_LOGI(TAG, "time synchronized from %s", MEMORY_CLOCK_TIME_SERVER);
    ESP_ERROR_CHECK(buttons_init());

    banner_render_status(banner_buffer, sizeof(banner_buffer), "Loading pages",
                         MEMORY_CLOCK_SERVER_URL);
    ESP_ERROR_CHECK(display_port_show_monochrome_full(banner_buffer, sizeof(banner_buffer),
                                                      BANNER_WIDTH, BANNER_HEIGHT));
    BaseType_t task_created = xTaskCreate(server_poll_task, "server_poll",
                                          POLL_TASK_STACK_BYTES, NULL,
                                          POLL_TASK_PRIORITY, NULL);
    ESP_ERROR_CHECK(task_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    int last_minute = -1;
    size_t page_count = banner_page_count();
    size_t current_page = 0;
    bool force_full_refresh = true;
    banner_clock_layout_t clock_layout = {0};
    ESP_LOGI(TAG, "configured %u display page(s)", (unsigned)page_count);
    while(true) {
        bool reset_page = false;
        if(take_server_update(&reset_page)) {
            if(reset_page) current_page = 0;
            page_count = banner_page_count();
            force_full_refresh = true;
            ESP_LOGI(TAG, "server page state changed; redrawing page %u/%u",
                     (unsigned)(current_page + 1), (unsigned)page_count);
        }

        page_count = banner_page_count();
        if(current_page >= page_count) {
            current_page = 0;
            force_full_refresh = true;
        }

        if(apply_pending_navigation(&current_page, page_count)) {
            force_full_refresh = true;
        }

        time_t now = time(NULL);
        struct tm tm_info;
        localtime_r(&now, &tm_info);

        bool minute_changed = tm_info.tm_min != last_minute;
        status_set_wifi_connected(provisioning_is_connected());
        if(status_take_changed()) force_full_refresh = true;
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

        if(apply_pending_navigation(&current_page, page_count)) {
            force_full_refresh = true;
            continue;
        } else {
            vTaskDelay(pdMS_TO_TICKS(LOOP_POLL_MS));
        }
    }
}
