#include "status.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum {
    BATTERY_ADC_GPIO = 1,
    BATTERY_ENABLE_GPIO = 21,
    BATTERY_SAMPLE_COUNT = 9,
    BATTERY_SETTLE_MS = 100,
    BATTERY_DIVIDER_MULTIPLIER = 2,
    BATTERY_MIN_VALID_MV = 3000,
    BATTERY_MAX_VALID_MV = 4500,
    BATTERY_LOW_MV = 3500,
    BATTERY_CLEAR_MV = 3600,
};

static const char *TAG = "status";
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
static adc_oneshot_unit_handle_t battery_adc;
static adc_cali_handle_t battery_cali;
static bool wifi_connected;
static bool server_known;
static bool server_reachable;
static bool battery_known;
static bool battery_low;
static uint32_t last_flags;
static bool changed;

static uint32_t current_flags_locked(void)
{
    uint32_t flags = 0;
    if(!wifi_connected) {
        flags |= STATUS_FLAG_WIFI_OFF;
    } else if(server_known && !server_reachable) {
        flags |= STATUS_FLAG_CLOUD_OFF;
    }
    if(battery_known && battery_low) {
        flags |= STATUS_FLAG_BATTERY_LOW;
    }
    return flags;
}

static bool update_changed_locked(void)
{
    uint32_t flags = current_flags_locked();
    if(flags == last_flags) return false;
    last_flags = flags;
    changed = true;
    return true;
}

static int median_sample_mv(int samples[], size_t count)
{
    for(size_t i = 1; i < count; ++i) {
        int value = samples[i];
        size_t j = i;
        while(j > 0 && samples[j - 1] > value) {
            samples[j] = samples[j - 1];
            --j;
        }
        samples[j] = value;
    }
    return samples[count / 2];
}

esp_err_t status_init(void)
{
    gpio_config_t enable_config = {
        .pin_bit_mask = 1ULL << BATTERY_ENABLE_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&enable_config), TAG, "battery enable gpio");
    ESP_RETURN_ON_ERROR(gpio_set_level(BATTERY_ENABLE_GPIO, 0), TAG, "battery monitor off");

    adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_config, &battery_adc), TAG, "battery adc");

    adc_unit_t unit;
    adc_channel_t channel;
    ESP_RETURN_ON_ERROR(adc_oneshot_io_to_channel(BATTERY_ADC_GPIO, &unit, &channel), TAG,
                        "battery adc channel");
    if(unit != ADC_UNIT_1) return ESP_ERR_INVALID_STATE;
    adc_oneshot_chan_cfg_t channel_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(battery_adc, channel, &channel_config), TAG,
                        "battery adc channel config");
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t calibration_config = {
        .unit_id = unit,
        .chan = channel,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    esp_err_t calibration_err = adc_cali_create_scheme_curve_fitting(&calibration_config,
                                                                       &battery_cali);
    if(calibration_err != ESP_OK) {
        ESP_LOGW(TAG, "battery ADC calibration unavailable: %s",
                 esp_err_to_name(calibration_err));
    }
#endif
    return ESP_OK;
}

bool status_set_wifi_connected(bool connected)
{
    portENTER_CRITICAL(&status_lock);
    wifi_connected = connected;
    bool did_change = update_changed_locked();
    portEXIT_CRITICAL(&status_lock);
    return did_change;
}

bool status_set_server_reachable(bool reachable)
{
    portENTER_CRITICAL(&status_lock);
    server_known = true;
    server_reachable = reachable;
    bool did_change = update_changed_locked();
    portEXIT_CRITICAL(&status_lock);
    return did_change;
}

bool status_sample_battery(void)
{
    if(battery_adc == NULL) return false;

    adc_unit_t unit;
    adc_channel_t channel;
    if(adc_oneshot_io_to_channel(BATTERY_ADC_GPIO, &unit, &channel) != ESP_OK) return false;

    gpio_set_level(BATTERY_ENABLE_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(BATTERY_SETTLE_MS));
    int samples[BATTERY_SAMPLE_COUNT];
    size_t sample_count = 0;
    for(size_t i = 0; i < BATTERY_SAMPLE_COUNT; ++i) {
        int adc_mv = 0;
        esp_err_t err;
        if(battery_cali != NULL) {
            err = adc_oneshot_get_calibrated_result(battery_adc, battery_cali, channel, &adc_mv);
        } else {
            int raw = 0;
            err = adc_oneshot_read(battery_adc, channel, &raw);
            adc_mv = (raw * 3100) / 4095;
        }
        if(err == ESP_OK) samples[sample_count++] = adc_mv;
    }
    gpio_set_level(BATTERY_ENABLE_GPIO, 0);
    if(sample_count == 0) {
        ESP_LOGW(TAG, "battery ADC read failed");
        return false;
    }

    int adc_mv = median_sample_mv(samples, sample_count);
    int battery_mv = adc_mv * BATTERY_DIVIDER_MULTIPLIER;
    if(battery_mv < BATTERY_MIN_VALID_MV || battery_mv > BATTERY_MAX_VALID_MV) {
        ESP_LOGW(TAG, "discarding implausible battery sample: cell=%d mV", battery_mv);
        return false;
    }
    ESP_LOGI(TAG, "battery sample: adc=%d mV cell=%d mV", adc_mv, battery_mv);

    portENTER_CRITICAL(&status_lock);
    battery_known = true;
    if(battery_low) {
        battery_low = battery_mv < BATTERY_CLEAR_MV;
    } else {
        battery_low = battery_mv <= BATTERY_LOW_MV;
    }
    bool did_change = update_changed_locked();
    portEXIT_CRITICAL(&status_lock);
    return did_change;
}

uint32_t status_flags(void)
{
    portENTER_CRITICAL(&status_lock);
    uint32_t flags = current_flags_locked();
    portEXIT_CRITICAL(&status_lock);
    return flags;
}

bool status_take_changed(void)
{
    portENTER_CRITICAL(&status_lock);
    bool was_changed = changed;
    changed = false;
    portEXIT_CRITICAL(&status_lock);
    return was_changed;
}
