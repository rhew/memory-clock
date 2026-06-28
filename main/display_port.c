#include "display_port.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "display_port";

enum {
    E1001_WIDTH = 800,
    E1001_HEIGHT = 480,
    E1001_PIN_SCLK = 7,
    E1001_PIN_MOSI = 9,
    E1001_PIN_CS = 10,
    E1001_PIN_DC = 11,
    E1001_PIN_RST = 12,
    E1001_PIN_BUSY = 13,
    E1001_SPI_HZ = 10 * 1000 * 1000,
    E1001_REFRESH_TIMEOUT_MS = 60000,
};

static spi_device_handle_t display_spi;
static uint8_t previous_frame[E1001_WIDTH * E1001_HEIGHT / 8];
static bool have_previous_frame;
static bool panel_awake;

static const uint8_t cmd_user[] = {0x17, 0x3f, 0x3f, 0x09, 0x06, 0x16};
static const uint8_t lut_vcom[42] = {
    0x26, 0x0f, 0x18, 0x18, 0x14, 0x01, 0x00, 0x0a, 0x00, 0x00, 0x00,
    0x01,
};
static const uint8_t lut_ww[42] = {
    0x55, 0x06, 0x0c, 0x17, 0x02, 0x01, 0x2a, 0x02, 0x1c, 0x02, 0x0d,
    0x01, 0x80, 0x02,
};
static const uint8_t lut_kw[42] = {
    0x55, 0x06, 0x0c, 0x17, 0x02, 0x01, 0x2a, 0x02, 0x1c, 0x02, 0x0d,
    0x01, 0x80, 0x02,
};
static const uint8_t lut_wk[42] = {
    0xaa, 0x06, 0x0c, 0x17, 0x02, 0x01, 0x15, 0x02, 0x1c, 0x02, 0x0d,
    0x01, 0x40, 0x02,
};
static const uint8_t lut_kk[42] = {
    0xaa, 0x06, 0x0c, 0x17, 0x02, 0x01, 0x15, 0x02, 0x1c, 0x02, 0x0d,
    0x01, 0x40, 0x02,
};

static void delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static esp_err_t wait_ready(uint32_t timeout_ms)
{
    int64_t start = esp_timer_get_time();
    while(gpio_get_level(E1001_PIN_BUSY) == 0) {
        if((esp_timer_get_time() - start) / 1000 > timeout_ms) {
            return ESP_ERR_TIMEOUT;
        }
        delay_ms(10);
    }
    return ESP_OK;
}

static esp_err_t spi_write_bytes(const uint8_t *data, size_t length)
{
    while(length > 0) {
        size_t chunk = length > 4096 ? 4096 : length;
        spi_transaction_t transaction = {
            .length = chunk * 8,
            .tx_buffer = data,
        };
        ESP_RETURN_ON_ERROR(spi_device_polling_transmit(display_spi, &transaction), TAG,
                            "spi transmit");
        data += chunk;
        length -= chunk;
    }
    return ESP_OK;
}

static esp_err_t write_command(uint8_t command)
{
    gpio_set_level(E1001_PIN_DC, 0);
    return spi_write_bytes(&command, 1);
}

static esp_err_t write_data(const uint8_t *data, size_t length)
{
    gpio_set_level(E1001_PIN_DC, 1);
    return spi_write_bytes(data, length);
}

static esp_err_t write_data_byte(uint8_t data)
{
    return write_data(&data, 1);
}

static esp_err_t write_lut(void)
{
    ESP_RETURN_ON_ERROR(wait_ready(E1001_REFRESH_TIMEOUT_MS), TAG, "lut busy");
    ESP_RETURN_ON_ERROR(write_command(0x20), TAG, "lut vcom command");
    ESP_RETURN_ON_ERROR(write_data(lut_vcom, sizeof(lut_vcom)), TAG, "lut vcom");

    ESP_RETURN_ON_ERROR(wait_ready(E1001_REFRESH_TIMEOUT_MS), TAG, "lut ww busy");
    ESP_RETURN_ON_ERROR(write_command(0x21), TAG, "lut ww command");
    ESP_RETURN_ON_ERROR(write_data(lut_ww, sizeof(lut_ww)), TAG, "lut ww");

    ESP_RETURN_ON_ERROR(wait_ready(E1001_REFRESH_TIMEOUT_MS), TAG, "lut kw busy");
    ESP_RETURN_ON_ERROR(write_command(0x22), TAG, "lut kw command");
    ESP_RETURN_ON_ERROR(write_data(lut_kw, sizeof(lut_kw)), TAG, "lut kw");

    ESP_RETURN_ON_ERROR(wait_ready(E1001_REFRESH_TIMEOUT_MS), TAG, "lut wk busy");
    ESP_RETURN_ON_ERROR(write_command(0x23), TAG, "lut wk command");
    ESP_RETURN_ON_ERROR(write_data(lut_wk, sizeof(lut_wk)), TAG, "lut wk");

    ESP_RETURN_ON_ERROR(write_command(0x24), TAG, "lut kk command");
    ESP_RETURN_ON_ERROR(write_data(lut_kk, sizeof(lut_kk)), TAG, "lut kk");
    return ESP_OK;
}

static esp_err_t reset_panel(void)
{
    gpio_set_level(E1001_PIN_RST, 0);
    delay_ms(10);
    gpio_set_level(E1001_PIN_RST, 1);
    delay_ms(10);
    esp_err_t err = wait_ready(E1001_REFRESH_TIMEOUT_MS);
    if(err == ESP_OK) panel_awake = false;
    return err;
}

static esp_err_t init_panel(void)
{
    ESP_RETURN_ON_ERROR(write_command(0x01), TAG, "power setting");
    ESP_RETURN_ON_ERROR(write_data_byte(0x07), TAG, "power setting");
    ESP_RETURN_ON_ERROR(write_data_byte(cmd_user[0]), TAG, "power setting");
    ESP_RETURN_ON_ERROR(write_data_byte(cmd_user[1]), TAG, "power setting");
    ESP_RETURN_ON_ERROR(write_data_byte(cmd_user[2]), TAG, "power setting");
    ESP_RETURN_ON_ERROR(write_data_byte(cmd_user[3]), TAG, "power setting");

    ESP_RETURN_ON_ERROR(write_command(0x30), TAG, "pll control");
    ESP_RETURN_ON_ERROR(write_data_byte(cmd_user[4]), TAG, "pll control");

    ESP_RETURN_ON_ERROR(write_command(0x82), TAG, "vcom dc setting");
    ESP_RETURN_ON_ERROR(write_data_byte(cmd_user[5]), TAG, "vcom dc setting");

    ESP_RETURN_ON_ERROR(write_command(0x06), TAG, "booster soft start");
    ESP_RETURN_ON_ERROR(write_data_byte(0x17), TAG, "booster soft start");
    ESP_RETURN_ON_ERROR(write_data_byte(0x17), TAG, "booster soft start");
    ESP_RETURN_ON_ERROR(write_data_byte(0x28), TAG, "booster soft start");
    ESP_RETURN_ON_ERROR(write_data_byte(0x17), TAG, "booster soft start");

    ESP_RETURN_ON_ERROR(write_command(0x04), TAG, "power on");
    delay_ms(100);
    ESP_RETURN_ON_ERROR(wait_ready(E1001_REFRESH_TIMEOUT_MS), TAG, "power on busy");

    ESP_RETURN_ON_ERROR(write_command(0x00), TAG, "panel setting");
    ESP_RETURN_ON_ERROR(write_data_byte(0x3f), TAG, "panel setting");

    ESP_RETURN_ON_ERROR(write_command(0x61), TAG, "resolution");
    ESP_RETURN_ON_ERROR(write_data_byte(E1001_WIDTH >> 8), TAG, "resolution");
    ESP_RETURN_ON_ERROR(write_data_byte(E1001_WIDTH & 0xff), TAG, "resolution");
    ESP_RETURN_ON_ERROR(write_data_byte(E1001_HEIGHT >> 8), TAG, "resolution");
    ESP_RETURN_ON_ERROR(write_data_byte(E1001_HEIGHT & 0xff), TAG, "resolution");

    ESP_RETURN_ON_ERROR(write_command(0x50), TAG, "vcom interval");
    ESP_RETURN_ON_ERROR(write_data_byte(0x10), TAG, "vcom interval");
    ESP_RETURN_ON_ERROR(write_data_byte(0x07), TAG, "vcom interval");
    return write_lut();
}

static esp_err_t set_full_window(void)
{
    ESP_RETURN_ON_ERROR(write_command(0x50), TAG, "window vcom");
    ESP_RETURN_ON_ERROR(write_data_byte(0xa9), TAG, "window vcom");
    ESP_RETURN_ON_ERROR(write_data_byte(0x07), TAG, "window vcom");
    ESP_RETURN_ON_ERROR(write_command(0x91), TAG, "partial in");
    ESP_RETURN_ON_ERROR(write_command(0x90), TAG, "partial window");
    ESP_RETURN_ON_ERROR(write_data_byte(0x00), TAG, "x start");
    ESP_RETURN_ON_ERROR(write_data_byte(0x00), TAG, "x start");
    ESP_RETURN_ON_ERROR(write_data_byte((E1001_WIDTH - 1) >> 8), TAG, "x end");
    ESP_RETURN_ON_ERROR(write_data_byte((E1001_WIDTH - 1) & 0xff), TAG, "x end");
    ESP_RETURN_ON_ERROR(write_data_byte(0x00), TAG, "y start");
    ESP_RETURN_ON_ERROR(write_data_byte(0x00), TAG, "y start");
    ESP_RETURN_ON_ERROR(write_data_byte((E1001_HEIGHT - 1) >> 8), TAG, "y end");
    ESP_RETURN_ON_ERROR(write_data_byte((E1001_HEIGHT - 1) & 0xff), TAG, "y end");
    ESP_RETURN_ON_ERROR(write_data_byte(0x01), TAG, "scan");
    return ESP_OK;
}

static esp_err_t set_partial_window(int x, int y, int width, int height)
{
    int x_start = x & ~7;
    int x_end = (x + width + 7) & ~7;
    int y_end = y + height;

    if(x_start < 0 || y < 0 || x_end > E1001_WIDTH || y_end > E1001_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(write_command(0x50), TAG, "window vcom");
    ESP_RETURN_ON_ERROR(write_data_byte(0xa9), TAG, "window vcom");
    ESP_RETURN_ON_ERROR(write_data_byte(0x07), TAG, "window vcom");
    ESP_RETURN_ON_ERROR(write_command(0x91), TAG, "partial in");
    ESP_RETURN_ON_ERROR(write_command(0x90), TAG, "partial window");
    ESP_RETURN_ON_ERROR(write_data_byte((uint8_t)(x_start >> 8)), TAG, "x start");
    ESP_RETURN_ON_ERROR(write_data_byte((uint8_t)(x_start & 0xff)), TAG, "x start");
    ESP_RETURN_ON_ERROR(write_data_byte((uint8_t)((x_end - 1) >> 8)), TAG, "x end");
    ESP_RETURN_ON_ERROR(write_data_byte((uint8_t)((x_end - 1) & 0xff)), TAG, "x end");
    ESP_RETURN_ON_ERROR(write_data_byte((uint8_t)(y >> 8)), TAG, "y start");
    ESP_RETURN_ON_ERROR(write_data_byte((uint8_t)(y & 0xff)), TAG, "y start");
    ESP_RETURN_ON_ERROR(write_data_byte((uint8_t)((y_end - 1) >> 8)), TAG, "y end");
    ESP_RETURN_ON_ERROR(write_data_byte((uint8_t)((y_end - 1) & 0xff)), TAG, "y end");
    ESP_RETURN_ON_ERROR(write_data_byte(0x01), TAG, "scan");
    return ESP_OK;
}

static esp_err_t refresh_panel(void)
{
    ESP_RETURN_ON_ERROR(write_command(0x12), TAG, "display refresh");
    delay_ms(1);
    return wait_ready(E1001_REFRESH_TIMEOUT_MS);
}

static esp_err_t sleep_panel(void)
{
    ESP_RETURN_ON_ERROR(write_command(0x50), TAG, "sleep vcom");
    ESP_RETURN_ON_ERROR(write_data_byte(0xf7), TAG, "sleep vcom");
    ESP_RETURN_ON_ERROR(write_command(0x02), TAG, "power off");
    ESP_RETURN_ON_ERROR(wait_ready(E1001_REFRESH_TIMEOUT_MS), TAG, "power off busy");
    ESP_RETURN_ON_ERROR(write_command(0x07), TAG, "deep sleep");
    ESP_RETURN_ON_ERROR(write_data_byte(0xa5), TAG, "deep sleep");
    panel_awake = false;
    return ESP_OK;
}

static esp_err_t ensure_panel_awake(void)
{
    if(panel_awake) return ESP_OK;

    ESP_RETURN_ON_ERROR(reset_panel(), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(init_panel(), TAG, "panel init");
    panel_awake = true;
    return ESP_OK;
}

static void copy_region(uint8_t *dest, const uint8_t *src, int x, int y, int width, int height)
{
    int x_start = x & ~7;
    int x_end = (x + width + 7) & ~7;
    int bytes_per_row = (x_end - x_start) / 8;
    int start_byte = x_start / 8;
    int full_stride = E1001_WIDTH / 8;

    for(int row = 0; row < height; ++row) {
        const uint8_t *src_row = src + (size_t)(y + row) * full_stride + start_byte;
        memcpy(dest + (size_t)row * bytes_per_row, src_row, bytes_per_row);
    }
}

static bool find_changed_region(const uint8_t *old_frame, const uint8_t *new_frame,
                                int x, int y, int width, int height,
                                int *out_x, int *out_y, int *out_width, int *out_height)
{
    int x_start = x & ~7;
    int x_end = (x + width + 7) & ~7;
    int start_byte = x_start / 8;
    int end_byte = x_end / 8;
    int stride = E1001_WIDTH / 8;
    int min_byte = end_byte;
    int max_byte = -1;
    int min_row = y + height;
    int max_row = -1;

    for(int row = y; row < y + height; ++row) {
        const uint8_t *old_row = old_frame + (size_t)row * stride;
        const uint8_t *new_row = new_frame + (size_t)row * stride;
        for(int byte = start_byte; byte < end_byte; ++byte) {
            if(old_row[byte] != new_row[byte]) {
                if(byte < min_byte) min_byte = byte;
                if(byte > max_byte) max_byte = byte;
                if(row < min_row) min_row = row;
                if(row > max_row) max_row = row;
            }
        }
    }

    if(max_byte < min_byte || max_row < min_row) {
        return false;
    }

    *out_x = min_byte * 8;
    *out_y = min_row;
    *out_width = (max_byte - min_byte + 1) * 8;
    *out_height = max_row - min_row + 1;
    return true;
}


esp_err_t display_port_init(void)
{
    if(display_spi != NULL) return ESP_OK;

    gpio_config_t output_config = {
        .pin_bit_mask = (1ULL << E1001_PIN_DC) | (1ULL << E1001_PIN_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&output_config), TAG, "gpio output config");

    gpio_config_t input_config = {
        .pin_bit_mask = (1ULL << E1001_PIN_BUSY),
        .mode = GPIO_MODE_INPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&input_config), TAG, "gpio input config");

    spi_bus_config_t bus_config = {
        .mosi_io_num = E1001_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = E1001_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO), TAG,
                        "spi bus init");

    spi_device_interface_config_t device_config = {
        .clock_speed_hz = E1001_SPI_HZ,
        .mode = 0,
        .spics_io_num = E1001_PIN_CS,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(SPI2_HOST, &device_config, &display_spi), TAG,
                        "spi add device");

    ESP_LOGI(TAG, "wired E1001 UC8179 display on SPI pins sclk=%d mosi=%d cs=%d dc=%d rst=%d busy=%d",
             E1001_PIN_SCLK, E1001_PIN_MOSI, E1001_PIN_CS, E1001_PIN_DC,
             E1001_PIN_RST, E1001_PIN_BUSY);
    return ESP_OK;
}

esp_err_t display_port_show_monochrome_full(const uint8_t *buffer, size_t buffer_size,
                                            int width, int height)
{
    if(buffer == NULL || width != E1001_WIDTH || height != E1001_HEIGHT
       || buffer_size < (E1001_WIDTH * E1001_HEIGHT / 8)) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(display_port_init(), TAG, "display init");
    ESP_LOGI(TAG, "refreshing E1001 UC8179 display with %dx%d monochrome frame",
             width, height);

    ESP_RETURN_ON_ERROR(ensure_panel_awake(), TAG, "panel wake");
    ESP_RETURN_ON_ERROR(set_full_window(), TAG, "set full window");

    ESP_RETURN_ON_ERROR(write_command(0x10), TAG, "old frame");
    ESP_RETURN_ON_ERROR(write_data(buffer, E1001_WIDTH * E1001_HEIGHT / 8), TAG, "old frame");

    ESP_RETURN_ON_ERROR(write_command(0x13), TAG, "new frame");
    ESP_RETURN_ON_ERROR(write_data(buffer, E1001_WIDTH * E1001_HEIGHT / 8), TAG, "new frame");

    ESP_RETURN_ON_ERROR(refresh_panel(), TAG, "panel refresh");
    ESP_RETURN_ON_ERROR(sleep_panel(), TAG, "panel sleep");
    memcpy(previous_frame, buffer, E1001_WIDTH * E1001_HEIGHT / 8);
    have_previous_frame = true;
    return ESP_OK;
}

esp_err_t display_port_show_monochrome_partial(const uint8_t *buffer, size_t buffer_size,
                                               int width, int height,
                                               int x, int y, int region_width, int region_height)
{
    if(buffer == NULL || width != E1001_WIDTH || height != E1001_HEIGHT
       || buffer_size < (E1001_WIDTH * E1001_HEIGHT / 8)) {
        return ESP_ERR_INVALID_ARG;
    }
    if(region_width <= 0 || region_height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if(!have_previous_frame) {
        return display_port_show_monochrome_full(buffer, buffer_size, width, height);
    }

    int changed_x;
    int changed_y;
    int changed_width;
    int changed_height;
    if(!find_changed_region(previous_frame, buffer, x, y, region_width, region_height,
                            &changed_x, &changed_y, &changed_width, &changed_height)) {
        return ESP_OK;
    }

    esp_err_t err = display_port_init();
    if(err != ESP_OK) {
        return err;
    }
    int x_start = changed_x & ~7;
    int x_end = (changed_x + changed_width + 7) & ~7;
    int region_bytes_per_row = (x_end - x_start) / 8;
    int total_region_bytes = region_bytes_per_row * changed_height;
    uint8_t *new_region = malloc(total_region_bytes);
    if(new_region == NULL) {
        return ESP_ERR_NO_MEM;
    }
    copy_region(new_region, buffer, changed_x, changed_y, changed_width, changed_height);

    ESP_LOGI(TAG, "partial refresh E1001 UC8179 region x=%d y=%d w=%d h=%d",
             x_start, changed_y, x_end - x_start, changed_height);

    err = ensure_panel_awake();
    if(err != ESP_OK) {
        free(new_region);
        return err;
    }

    err = set_partial_window(changed_x, changed_y, changed_width, changed_height);
    if(err != ESP_OK) {
        free(new_region);
        return err;
    }

    err = write_command(0x13);
    if(err != ESP_OK) {
        free(new_region);
        return err;
    }
    err = write_data(new_region, total_region_bytes);
    free(new_region);
    if(err != ESP_OK) {
        return err;
    }

    err = refresh_panel();
    if(err != ESP_OK) {
        return err;
    }
    err = write_command(0x92);
    if(err != ESP_OK) {
        return err;
    }

    memcpy(previous_frame, buffer, E1001_WIDTH * E1001_HEIGHT / 8);
    have_previous_frame = true;
    return ESP_OK;
}
