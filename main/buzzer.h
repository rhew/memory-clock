#pragma once

#include "alert_tone.h"
#include "esp_err.h"

esp_err_t buzzer_init(void);
esp_err_t buzzer_play(const memory_clock_alert_sequence_t *sequence);
void buzzer_stop(void);
