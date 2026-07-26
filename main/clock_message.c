#include "clock_message.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "clock_message";
static const char *NVS_NAMESPACE = "clock_message";
static const char *NVS_DISMISSED_KEY = "dismissed_id";
static SemaphoreHandle_t message_lock;
static bool active;
static bool displayed;
static bool changed;
static memory_clock_alert_sequence_t active_alert;
static char active_id[CLOCK_MESSAGE_ID_CAPACITY];
static char active_text[CLOCK_MESSAGE_TEXT_CAPACITY];
static char dismissed_id[CLOCK_MESSAGE_ID_CAPACITY];

static void persist_dismissed_id(const char *message_id)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "failed to open dismissed-message state: %s", esp_err_to_name(err));
        return;
    }
    if(message_id != NULL && message_id[0] != '\0') {
        err = nvs_set_str(handle, NVS_DISMISSED_KEY, message_id);
    } else {
        err = nvs_erase_key(handle, NVS_DISMISSED_KEY);
        if(err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if(err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "failed to persist dismissed-message state: %s",
                 esp_err_to_name(err));
    }
}

esp_err_t clock_message_init(void)
{
    if(message_lock != NULL) return ESP_OK;
    message_lock = xSemaphoreCreateMutex();
    if(message_lock == NULL) return ESP_ERR_NO_MEM;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if(err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "failed to read dismissed-message state: %s", esp_err_to_name(err));
        return ESP_OK;
    }
    size_t size = sizeof(dismissed_id);
    err = nvs_get_str(handle, NVS_DISMISSED_KEY, dismissed_id, &size);
    nvs_close(handle);
    if(err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if(err != ESP_OK || strnlen(dismissed_id, sizeof(dismissed_id))
                        >= sizeof(dismissed_id)) {
        dismissed_id[0] = '\0';
        ESP_LOGW(TAG, "ignored invalid dismissed-message state");
    }
    return ESP_OK;
}

esp_err_t clock_message_receive(const char *message_id, const char *text,
                                const memory_clock_alert_sequence_t *alert)
{
    if(message_lock == NULL) return ESP_ERR_INVALID_STATE;
    if(message_id == NULL || text == NULL || alert == NULL) return ESP_ERR_INVALID_ARG;
    size_t id_length = strnlen(message_id, CLOCK_MESSAGE_ID_CAPACITY);
    size_t text_length = strnlen(text, CLOCK_MESSAGE_TEXT_CAPACITY);
    if(id_length == 0 || id_length >= CLOCK_MESSAGE_ID_CAPACITY
       || text_length == 0 || text_length >= CLOCK_MESSAGE_TEXT_CAPACITY) {
        return ESP_ERR_INVALID_SIZE;
    }
    if(alert->count > MEMORY_CLOCK_ALERT_MAX_TONES) return ESP_ERR_INVALID_SIZE;
    uint32_t total_ms = 0;
    for(size_t i = 0; i < alert->count; ++i) {
        const memory_clock_alert_tone_t *tone = &alert->tones[i];
        if(tone->frequency_hz < MEMORY_CLOCK_ALERT_MIN_FREQUENCY_HZ
           || tone->frequency_hz > MEMORY_CLOCK_ALERT_MAX_FREQUENCY_HZ
           || tone->duration_ms < MEMORY_CLOCK_ALERT_MIN_DURATION_MS
           || tone->duration_ms > MEMORY_CLOCK_ALERT_MAX_DURATION_MS
           || tone->gap_ms > MEMORY_CLOCK_ALERT_MAX_GAP_MS) {
            return ESP_ERR_INVALID_ARG;
        }
        total_ms += tone->duration_ms + tone->gap_ms;
        if(total_ms > MEMORY_CLOCK_ALERT_MAX_TOTAL_MS) return ESP_ERR_INVALID_SIZE;
    }

    xSemaphoreTake(message_lock, portMAX_DELAY);
    bool different = !active || strcmp(active_id, message_id) != 0
                     || strcmp(active_text, text) != 0
                     || active_alert.count != alert->count
                     || memcmp(active_alert.tones, alert->tones,
                               alert->count * sizeof(alert->tones[0])) != 0;
    bool clear_persisted_dismissal = false;
    if(different) {
        strlcpy(active_id, message_id, sizeof(active_id));
        strlcpy(active_text, text, sizeof(active_text));
        clear_persisted_dismissal = dismissed_id[0] != '\0';
        dismissed_id[0] = '\0';
        active = true;
        displayed = false;
        active_alert = *alert;
        changed = true;
    }
    xSemaphoreGive(message_lock);
    if(clear_persisted_dismissal) persist_dismissed_id(NULL);
    return ESP_OK;
}

bool clock_message_cancel_active(void)
{
    if(message_lock == NULL) return false;

    xSemaphoreTake(message_lock, portMAX_DELAY);
    bool did_cancel = active;
    if(did_cancel) {
        active_id[0] = '\0';
        active_text[0] = '\0';
        active = false;
        displayed = false;
        active_alert.count = 0;
        changed = true;
    }
    xSemaphoreGive(message_lock);
    return did_cancel;
}

bool clock_message_snapshot(char *message_id, size_t message_id_size,
                            char *text, size_t text_size)
{
    if(message_lock == NULL) return false;

    xSemaphoreTake(message_lock, portMAX_DELAY);
    bool have_message = active;
    if(have_message) {
        if(message_id != NULL && message_id_size > 0) {
            strlcpy(message_id, active_id, message_id_size);
        }
        if(text != NULL && text_size > 0) {
            strlcpy(text, active_text, text_size);
        }
    }
    xSemaphoreGive(message_lock);
    return have_message;
}

bool clock_message_mark_displayed(const char *message_id)
{
    if(message_lock == NULL || message_id == NULL) return false;

    xSemaphoreTake(message_lock, portMAX_DELAY);
    bool did_mark = active && !displayed && strcmp(active_id, message_id) == 0;
    if(did_mark) displayed = true;
    xSemaphoreGive(message_lock);
    return did_mark;
}

bool clock_message_dismiss(const char *message_id)
{
    if(message_lock == NULL || message_id == NULL) return false;

    xSemaphoreTake(message_lock, portMAX_DELAY);
    bool did_dismiss = active && strcmp(active_id, message_id) == 0;
    char dismissed[CLOCK_MESSAGE_ID_CAPACITY] = {0};
    if(did_dismiss) {
        strlcpy(dismissed_id, active_id, sizeof(dismissed_id));
        strlcpy(dismissed, active_id, sizeof(dismissed));
        active_id[0] = '\0';
        active_text[0] = '\0';
        active = false;
        displayed = false;
        active_alert.count = 0;
    }
    xSemaphoreGive(message_lock);
    if(did_dismiss) persist_dismissed_id(dismissed);
    return did_dismiss;
}

bool clock_message_alert_snapshot(memory_clock_alert_sequence_t *alert)
{
    if(message_lock == NULL || alert == NULL) return false;

    xSemaphoreTake(message_lock, portMAX_DELAY);
    bool have_alert = active && active_alert.count > 0;
    if(have_alert) *alert = active_alert;
    xSemaphoreGive(message_lock);
    return have_alert;
}

bool clock_message_take_changed(void)
{
    if(message_lock == NULL) return false;

    xSemaphoreTake(message_lock, portMAX_DELAY);
    bool was_changed = changed;
    changed = false;
    xSemaphoreGive(message_lock);
    return was_changed;
}

void clock_message_report(clock_message_report_t *report)
{
    if(report == NULL) return;
    memset(report, 0, sizeof(*report));
    if(message_lock == NULL) return;

    xSemaphoreTake(message_lock, portMAX_DELAY);
    if(active) {
        strlcpy(report->active_id, active_id, sizeof(report->active_id));
        if(displayed) {
            strlcpy(report->displayed_id, active_id, sizeof(report->displayed_id));
        }
    }
    strlcpy(report->dismissed_id, dismissed_id, sizeof(report->dismissed_id));
    xSemaphoreGive(message_lock);
}
