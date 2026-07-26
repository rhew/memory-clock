#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "alert_tone.h"
#include "esp_err.h"

#define CLOCK_MESSAGE_ID_CAPACITY 25
#define CLOCK_MESSAGE_TEXT_CAPACITY 241

typedef struct {
    char active_id[CLOCK_MESSAGE_ID_CAPACITY];
    char displayed_id[CLOCK_MESSAGE_ID_CAPACITY];
    char dismissed_id[CLOCK_MESSAGE_ID_CAPACITY];
} clock_message_report_t;

esp_err_t clock_message_init(void);
esp_err_t clock_message_receive(const char *message_id, const char *text,
                                const memory_clock_alert_sequence_t *alert);
bool clock_message_cancel_active(void);
bool clock_message_snapshot(char *message_id, size_t message_id_size,
                            char *text, size_t text_size);
bool clock_message_mark_displayed(const char *message_id);
bool clock_message_dismiss(const char *message_id);
bool clock_message_alert_snapshot(memory_clock_alert_sequence_t *alert);
bool clock_message_take_changed(void);
void clock_message_report(clock_message_report_t *report);
