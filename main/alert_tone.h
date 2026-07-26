#pragma once

#include <stddef.h>
#include <stdint.h>

#define MEMORY_CLOCK_ALERT_MAX_TONES 16
#define MEMORY_CLOCK_ALERT_MIN_FREQUENCY_HZ 500
#define MEMORY_CLOCK_ALERT_MAX_FREQUENCY_HZ 3000
#define MEMORY_CLOCK_ALERT_MIN_DURATION_MS 20
#define MEMORY_CLOCK_ALERT_MAX_DURATION_MS 1000
#define MEMORY_CLOCK_ALERT_MAX_GAP_MS 1000
#define MEMORY_CLOCK_ALERT_MAX_TOTAL_MS 5000

typedef struct {
    uint16_t frequency_hz;
    uint16_t duration_ms;
    uint16_t gap_ms;
} memory_clock_alert_tone_t;

typedef struct {
    memory_clock_alert_tone_t tones[MEMORY_CLOCK_ALERT_MAX_TONES];
    size_t count;
} memory_clock_alert_sequence_t;
