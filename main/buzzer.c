#include "buzzer.h"

#include <stdbool.h>

#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "buzzer";

enum {
    BUZZER_GPIO = 45,
    BUZZER_DUTY = 512,
    BUZZER_TASK_STACK_BYTES = 4096,
    BUZZER_TASK_PRIORITY = 1,
};

static const ledc_mode_t BUZZER_SPEED_MODE = LEDC_LOW_SPEED_MODE;
static const ledc_timer_t BUZZER_TIMER = LEDC_TIMER_0;
static const ledc_channel_t BUZZER_CHANNEL = LEDC_CHANNEL_0;
static QueueHandle_t command_queue;
static bool initialized;

static bool wait_for_replacement(uint16_t milliseconds,
                                 memory_clock_alert_sequence_t *replacement)
{
    if(milliseconds == 0) return false;
    return xQueueReceive(command_queue, replacement, pdMS_TO_TICKS(milliseconds))
           == pdTRUE;
}

static esp_err_t start_tone(const memory_clock_alert_tone_t *tone)
{
    ESP_RETURN_ON_ERROR(
        ledc_set_freq(BUZZER_SPEED_MODE, BUZZER_TIMER, tone->frequency_hz),
        TAG, "set buzzer frequency"
    );
    ESP_RETURN_ON_ERROR(ledc_set_duty(BUZZER_SPEED_MODE, BUZZER_CHANNEL,
                                     BUZZER_DUTY),
                        TAG, "set buzzer duty");
    return ledc_update_duty(BUZZER_SPEED_MODE, BUZZER_CHANNEL);
}

static bool valid_sequence(const memory_clock_alert_sequence_t *sequence)
{
    if(sequence == NULL || sequence->count == 0
       || sequence->count > MEMORY_CLOCK_ALERT_MAX_TONES) {
        return false;
    }
    uint32_t total_ms = 0;
    for(size_t i = 0; i < sequence->count; ++i) {
        const memory_clock_alert_tone_t *tone = &sequence->tones[i];
        if(tone->frequency_hz < MEMORY_CLOCK_ALERT_MIN_FREQUENCY_HZ
           || tone->frequency_hz > MEMORY_CLOCK_ALERT_MAX_FREQUENCY_HZ
           || tone->duration_ms < MEMORY_CLOCK_ALERT_MIN_DURATION_MS
           || tone->duration_ms > MEMORY_CLOCK_ALERT_MAX_DURATION_MS
           || tone->gap_ms > MEMORY_CLOCK_ALERT_MAX_GAP_MS) {
            return false;
        }
        total_ms += tone->duration_ms + tone->gap_ms;
        if(total_ms > MEMORY_CLOCK_ALERT_MAX_TOTAL_MS) return false;
    }
    return true;
}

static void buzzer_task(void *arg)
{
    (void)arg;
    memory_clock_alert_sequence_t sequence;
    while(true) {
        xQueueReceive(command_queue, &sequence, portMAX_DELAY);
        bool replaced = false;
        do {
            replaced = false;
            for(size_t i = 0; i < sequence.count; ++i) {
                esp_err_t err = start_tone(&sequence.tones[i]);
                if(err != ESP_OK) {
                    ESP_LOGW(TAG, "failed to start tone %u: %s",
                             (unsigned)i, esp_err_to_name(err));
                    sequence.count = 0;
                    break;
                }
                memory_clock_alert_sequence_t next;
                replaced = wait_for_replacement(sequence.tones[i].duration_ms, &next);
                ledc_stop(BUZZER_SPEED_MODE, BUZZER_CHANNEL, 0);
                if(replaced) {
                    sequence = next;
                    break;
                }
                replaced = wait_for_replacement(sequence.tones[i].gap_ms, &next);
                if(replaced) {
                    sequence = next;
                    break;
                }
            }
        } while(replaced && sequence.count > 0);
        ledc_stop(BUZZER_SPEED_MODE, BUZZER_CHANNEL, 0);
    }
}

esp_err_t buzzer_init(void)
{
    if(initialized) return ESP_OK;

    ledc_timer_config_t timer_config = {
        .speed_mode = BUZZER_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = BUZZER_TIMER,
        .freq_hz = 1000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_config), TAG,
                        "buzzer timer configuration");

    ledc_channel_config_t channel_config = {
        .gpio_num = BUZZER_GPIO,
        .speed_mode = BUZZER_SPEED_MODE,
        .channel = BUZZER_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BUZZER_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_config), TAG,
                        "buzzer channel configuration");

    command_queue = xQueueCreate(1, sizeof(memory_clock_alert_sequence_t));
    if(command_queue == NULL) return ESP_ERR_NO_MEM;
    BaseType_t created = xTaskCreate(
        buzzer_task, "buzzer", BUZZER_TASK_STACK_BYTES, NULL,
        BUZZER_TASK_PRIORITY, NULL
    );
    if(created != pdPASS) {
        vQueueDelete(command_queue);
        command_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    initialized = true;
    return ESP_OK;
}

esp_err_t buzzer_play(const memory_clock_alert_sequence_t *sequence)
{
    if(!initialized) return ESP_ERR_INVALID_STATE;
    if(!valid_sequence(sequence)) return ESP_ERR_INVALID_ARG;
    return xQueueOverwrite(command_queue, sequence) == pdPASS ? ESP_OK : ESP_FAIL;
}

void buzzer_stop(void)
{
    if(!initialized) return;
    memory_clock_alert_sequence_t stop = {0};
    xQueueOverwrite(command_queue, &stop);
}
