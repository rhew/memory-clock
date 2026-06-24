#include "ui.h"

#include <stdio.h>
#include <time.h>

static lv_obj_t *weekday_label;
static lv_obj_t *daypart_label;
static lv_obj_t *time_row;
static lv_obj_t *time_label;
static lv_obj_t *meridiem_label;
static lv_obj_t *date_label;

static const char *const WEEKDAY_NAMES[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};

static const char *const MONTH_NAMES[] = {
    "January",   "February", "March",    "April",   "May",      "June",
    "July",      "August",   "September","October", "November", "December"
};

static const char *time_of_day_for_hour(int hour)
{
    if(hour < 5) return "Night";
    if(hour < 12) return "Morning";
    if(hour < 17) return "Afternoon";
    if(hour < 21) return "Evening";
    return "Night";
}

static void update_clock(lv_timer_t *timer)
{
    LV_UNUSED(timer);

    time_t now = time(NULL);
    struct tm *local = localtime(&now);
    if(local == NULL) return;

    int hour12 = local->tm_hour % 12;
    char time_text[8];
    char date_text[32];

    if(hour12 == 0) hour12 = 12;

    snprintf(time_text, sizeof(time_text), "%d:%02d", hour12, local->tm_min);
    snprintf(date_text, sizeof(date_text), "%s %d, %d", MONTH_NAMES[local->tm_mon], local->tm_mday,
             local->tm_year + 1900);

    lv_label_set_text(weekday_label, WEEKDAY_NAMES[local->tm_wday]);
    lv_label_set_text(daypart_label, time_of_day_for_hour(local->tm_hour));
    lv_label_set_text(time_label, time_text);
    lv_label_set_text(meridiem_label, local->tm_hour < 12 ? "AM" : "PM");
    lv_label_set_text(date_label, date_text);
}

void ui_create(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    weekday_label = lv_label_create(screen);
    lv_obj_set_style_text_color(weekday_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(weekday_label, &lv_font_montserrat_48, 0);
    lv_obj_align(weekday_label, LV_ALIGN_CENTER, 0, -150);

    daypart_label = lv_label_create(screen);
    lv_obj_set_style_text_color(daypart_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(daypart_label, &lv_font_montserrat_24, 0);
    lv_obj_align(daypart_label, LV_ALIGN_CENTER, 0, -80);

    time_row = lv_obj_create(screen);
    lv_obj_remove_style_all(time_row);
    lv_obj_set_size(time_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(time_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(time_row, 8, 0);
    lv_obj_align(time_row, LV_ALIGN_CENTER, 0, 10);

    time_label = lv_label_create(time_row);
    lv_obj_set_style_text_color(time_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_48, 0);

    meridiem_label = lv_label_create(time_row);
    lv_obj_set_style_text_color(meridiem_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(meridiem_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_pad_top(meridiem_label, 16, 0);

    date_label = lv_label_create(screen);
    lv_obj_set_style_text_color(date_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(date_label, &lv_font_montserrat_24, 0);
    lv_obj_align(date_label, LV_ALIGN_CENTER, 0, 95);

    update_clock(NULL);
    lv_timer_create(update_clock, 1000, NULL);
}
