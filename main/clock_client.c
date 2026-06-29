#include "clock_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "wifi_env.h"

#include "image_store.h"

static const char *TAG = "clock_client";

enum {
    HTTP_TIMEOUT_MS = 15000,
    MAX_RESPONSE_BYTES = 1024 * 1024,
    READ_BUFFER_SIZE = 2048,
    URL_BUFFER_SIZE = 256,
};

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    char last_modified[96];
} http_response_t;

static char cached_last_modified[96];
static bool have_cached_images;

static void log_heap_state(const char *context)
{
    ESP_LOGW(TAG,
             "%s: free internal=%u largest internal=%u free psram=%u largest psram=%u",
             context,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static uint8_t *alloc_image_bits(size_t byte_count, int image_index, const char *name)
{
    uint8_t *bits = heap_caps_malloc(byte_count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(bits == NULL) {
        ESP_LOGW(TAG, "image %d (%s): failed to allocate %u bytes in PSRAM",
                 image_index, name, (unsigned)byte_count);
        log_heap_state("PSRAM image allocation failed");
        bits = malloc(byte_count);
    }
    if(bits == NULL) {
        ESP_LOGW(TAG, "image %d (%s): failed to allocate %u bytes in fallback heap",
                 image_index, name, (unsigned)byte_count);
        log_heap_state("fallback image allocation failed");
    }
    return bits;
}

static esp_err_t build_absolute_url(const char *path, char *out, size_t out_size)
{
    if(strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0) {
        int written = snprintf(out, out_size, "%s", path);
        return written > 0 && (size_t)written < out_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }

    if(path[0] != '/') return ESP_ERR_INVALID_ARG;

    const char *scheme_end = strstr(MEMORY_CLOCK_SERVER_URL, "://");
    if(scheme_end == NULL) return ESP_ERR_INVALID_ARG;
    const char *host_start = scheme_end + 3;
    const char *path_start = strchr(host_start, '/');
    size_t origin_len = path_start != NULL ? (size_t)(path_start - MEMORY_CLOCK_SERVER_URL)
                                           : strlen(MEMORY_CLOCK_SERVER_URL);
    int written = snprintf(out, out_size, "%.*s%s", (int)origin_len,
                           MEMORY_CLOCK_SERVER_URL, path);
    return written > 0 && (size_t)written < out_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static void log_response_prefix(const char *response)
{
    char prefix[161];
    if(response == NULL) {
        ESP_LOGW(TAG, "response body is empty");
        return;
    }

    size_t length = strnlen(response, sizeof(prefix) - 1);
    memcpy(prefix, response, length);
    prefix[length] = '\0';
    for(size_t i = 0; i < length; ++i) {
        if((unsigned char)prefix[i] < 0x20) {
            prefix[i] = ' ';
        }
    }
    ESP_LOGW(TAG, "response prefix: %s", prefix);
}

static esp_err_t append_response_data(http_response_t *response, const char *data, int length)
{
    if(length <= 0) return ESP_OK;
    if(response->length + (size_t)length > MAX_RESPONSE_BYTES) {
        return ESP_ERR_NO_MEM;
    }

    size_t needed = response->length + (size_t)length + 1;
    if(needed > response->capacity) {
        size_t new_capacity = response->capacity == 0 ? 4096 : response->capacity;
        while(new_capacity < needed) {
            new_capacity *= 2;
        }
        char *new_data = realloc(response->data, new_capacity);
        if(new_data == NULL) return ESP_ERR_NO_MEM;
        response->data = new_data;
        response->capacity = new_capacity;
    }

    memcpy(response->data + response->length, data, (size_t)length);
    response->length += (size_t)length;
    response->data[response->length] = '\0';
    return ESP_OK;
}

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    http_response_t *response = event->user_data;
    switch(event->event_id) {
    case HTTP_EVENT_ON_HEADER:
        if(strcasecmp(event->header_key, "Last-Modified") == 0) {
            strlcpy(response->last_modified, event->header_value,
                    sizeof(response->last_modified));
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

static esp_err_t read_response_body(esp_http_client_handle_t client,
                                    http_response_t *response,
                                    int64_t content_length)
{
    char read_buffer[READ_BUFFER_SIZE];
    while(true) {
        int read_len = esp_http_client_read(client, read_buffer, sizeof(read_buffer));
        if(read_len > 0) {
            ESP_RETURN_ON_ERROR(append_response_data(response, read_buffer, read_len),
                                TAG, "append response data");
            continue;
        }
        if(read_len == 0) {
            break;
        }
        if(read_len == -ESP_ERR_HTTP_EAGAIN) {
            continue;
        }
        ESP_LOGW(TAG, "HTTP read failed: %d", read_len);
        return ESP_FAIL;
    }

    if(content_length >= 0 && response->length != (size_t)content_length) {
        ESP_LOGW(TAG, "HTTP body length mismatch: read=%u expected=%lld complete=%d",
                 (unsigned)response->length, content_length,
                 esp_http_client_is_complete_data_received(client));
        return ESP_ERR_INVALID_SIZE;
    }

    if(!esp_http_client_is_complete_data_received(client)) {
        ESP_LOGW(TAG, "HTTP response ended before complete body was received: read=%u expected=%lld",
                 (unsigned)response->length, content_length);
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t fetch_image_bits(const char *url, int width, int height,
                                  memory_clock_image_t *image, int image_index,
                                  const char *name)
{
    if(width <= 0 || height <= 0) {
        ESP_LOGW(TAG, "image %d (%s): invalid dimensions width=%d height=%d",
                 image_index, name, width, height);
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t expected_len = (size_t)((width + 7) / 8) * (size_t)height;
    uint8_t *bits = alloc_image_bits(expected_len, image_index, name);
    if(bits == NULL) return ESP_ERR_NO_MEM;

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if(client == NULL) {
        free(bits);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "Authorization", "Bearer " MEMORY_CLOCK_BEARER_TOKEN);

    esp_err_t err = esp_http_client_open(client, 0);
    int status = 0;
    int64_t content_length = -1;
    if(err == ESP_OK) {
        content_length = esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
    }
    if(err != ESP_OK || status != 200 || content_length != (int64_t)expected_len) {
        ESP_LOGW(TAG, "image %d (%s): fetch failed err=%s HTTP %d content_length=%lld expected=%u",
                 image_index, name, esp_err_to_name(err), status, content_length,
                 (unsigned)expected_len);
        esp_http_client_cleanup(client);
        free(bits);
        return err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE;
    }

    size_t total_read = 0;
    while(total_read < expected_len) {
        int read_len = esp_http_client_read(client, (char *)bits + total_read,
                                            (int)(expected_len - total_read));
        if(read_len > 0) {
            total_read += (size_t)read_len;
            continue;
        }
        if(read_len == -ESP_ERR_HTTP_EAGAIN) continue;

        ESP_LOGW(TAG, "image %d (%s): read failed at %u/%u with %d",
                 image_index, name, (unsigned)total_read, (unsigned)expected_len,
                 read_len);
        esp_http_client_cleanup(client);
        free(bits);
        return ESP_FAIL;
    }

    if(!esp_http_client_is_complete_data_received(client)) {
        ESP_LOGW(TAG, "image %d (%s): incomplete image body read=%u expected=%u",
                 image_index, name, (unsigned)total_read, (unsigned)expected_len);
        esp_http_client_cleanup(client);
        free(bits);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_cleanup(client);
    image->bits = bits;
    image->width = width;
    image->height = height;
    return ESP_OK;
}

static esp_err_t parse_images_json(const char *json, memory_clock_image_set_t *set)
{
    cJSON *root = cJSON_Parse(json);
    if(root == NULL) {
        const char *error = cJSON_GetErrorPtr();
        ESP_LOGW(TAG, "JSON parse failed near offset %u",
                 error != NULL && json != NULL ? (unsigned)(error - json) : 0);
        log_response_prefix(json);
        return ESP_ERR_INVALID_RESPONSE;
    }

    cJSON *images = cJSON_GetObjectItemCaseSensitive(root, "images");
    if(!cJSON_IsArray(images)) {
        ESP_LOGW(TAG, "JSON response missing images array");
        log_response_prefix(json);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    int count = cJSON_GetArraySize(images);
    memory_clock_image_t *parsed = NULL;
    if(count > 0) {
        parsed = calloc((size_t)count, sizeof(parsed[0]));
        if(parsed == NULL) {
            ESP_LOGW(TAG, "failed to allocate metadata for %d image(s)", count);
            log_heap_state("image metadata allocation failed");
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
    }

    int parsed_count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, images) {
        cJSON *name_item = cJSON_GetObjectItemCaseSensitive(item, "name");
        const char *name = cJSON_IsString(name_item) && name_item->valuestring != NULL
                               ? name_item->valuestring
                               : "(unnamed)";
        cJSON *bits_path = cJSON_GetObjectItemCaseSensitive(item, "bits_path");
        cJSON *width_item = cJSON_GetObjectItemCaseSensitive(item, "width");
        cJSON *height_item = cJSON_GetObjectItemCaseSensitive(item, "height");
        if(!cJSON_IsString(bits_path) || bits_path->valuestring == NULL) {
            ESP_LOGW(TAG, "image %d (%s): missing bits_path string", parsed_count, name);
            memory_clock_image_set_t partial = {
                .images = parsed,
                .count = (size_t)parsed_count,
            };
            image_store_free_set(&partial);
            cJSON_Delete(root);
            return ESP_ERR_INVALID_RESPONSE;
        }

        char image_url[URL_BUFFER_SIZE];
        esp_err_t err = build_absolute_url(bits_path->valuestring, image_url, sizeof(image_url));
        if(err == ESP_OK) {
            err = fetch_image_bits(image_url,
                                   cJSON_IsNumber(width_item) ? width_item->valueint : 0,
                                   cJSON_IsNumber(height_item) ? height_item->valueint : 0,
                                   &parsed[parsed_count], parsed_count, name);
        } else {
            ESP_LOGW(TAG, "image %d (%s): invalid bits_path URL: %s",
                     parsed_count, name, bits_path->valuestring);
        }
        if(err != ESP_OK) {
            memory_clock_image_set_t partial = {
                .images = parsed,
                .count = (size_t)parsed_count,
            };
            image_store_free_set(&partial);
            cJSON_Delete(root);
            return err;
        }
        ++parsed_count;
    }

    cJSON_Delete(root);
    set->images = parsed;
    set->count = (size_t)parsed_count;
    return ESP_OK;
}

clock_client_result_t clock_client_poll(void)
{
    http_response_t response = {0};
    esp_http_client_config_t config = {
        .url = MEMORY_CLOCK_SERVER_URL,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .event_handler = http_event_handler,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if(client == NULL) return CLOCK_CLIENT_ERROR;

    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_http_client_set_header(client, "Authorization", "Bearer " MEMORY_CLOCK_BEARER_TOKEN);
    if(have_cached_images && cached_last_modified[0] != '\0') {
        esp_http_client_set_header(client, "If-Modified-Since", cached_last_modified);
    }

    esp_err_t err = esp_http_client_open(client, 0);
    int status = 0;
    int64_t content_length = -1;
    if(err == ESP_OK) {
        content_length = esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        if(status == 304) {
            ESP_LOGI(TAG, "server pages not modified");
            esp_http_client_cleanup(client);
            return CLOCK_CLIENT_NOT_MODIFIED;
        }
        if(content_length < 0) {
            err = ESP_FAIL;
        } else if(content_length > MAX_RESPONSE_BYTES) {
            err = ESP_ERR_NO_MEM;
        } else {
            err = read_response_body(client, &response, content_length);
        }
    }
    esp_http_client_cleanup(client);

    if(err != ESP_OK) {
        ESP_LOGW(TAG, "poll failed: %s HTTP %d read=%u content_length=%lld",
                 esp_err_to_name(err), status, (unsigned)response.length, content_length);
        image_store_mark_server_unavailable_if_empty();
        free(response.data);
        return CLOCK_CLIENT_ERROR;
    }

    if(status != 200) {
        ESP_LOGW(TAG, "server returned HTTP %d", status);
        image_store_mark_server_unavailable_if_empty();
        free(response.data);
        return CLOCK_CLIENT_ERROR;
    }

    memory_clock_image_set_t replacement = {0};
    ESP_LOGI(TAG, "server returned HTTP %d, read=%u content_length=%lld, Last-Modified: %s",
             status, (unsigned)response.length, content_length,
             response.last_modified[0] != '\0' ? response.last_modified : "(none)");

    err = parse_images_json(response.data != NULL ? response.data : "", &replacement);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "failed to parse server pages: %s", esp_err_to_name(err));
        image_store_mark_server_unavailable_if_empty();
        free(response.data);
        return CLOCK_CLIENT_ERROR;
    }
    free(response.data);

    image_store_replace(&replacement);
    have_cached_images = true;
    if(response.last_modified[0] != '\0') {
        strlcpy(cached_last_modified, response.last_modified, sizeof(cached_last_modified));
    }
    ESP_LOGI(TAG, "loaded %u server page image(s)", (unsigned)image_store_count());
    return CLOCK_CLIENT_UPDATED;
}
