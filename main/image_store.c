#include "image_store.h"

#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static memory_clock_image_t *stored_images;
static size_t stored_image_count;
static image_store_status_t stored_status = IMAGE_STORE_SERVER_UNAVAILABLE;
static SemaphoreHandle_t store_mutex;

static void free_images(memory_clock_image_t *images, size_t count)
{
    if(images == NULL) return;
    for(size_t i = 0; i < count; ++i) {
        free(images[i].bits);
    }
    free(images);
}

esp_err_t image_store_init(void)
{
    if(store_mutex != NULL) return ESP_OK;
    store_mutex = xSemaphoreCreateRecursiveMutex();
    return store_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

void image_store_lock(void)
{
    if(store_mutex != NULL) {
        xSemaphoreTakeRecursive(store_mutex, portMAX_DELAY);
    }
}

void image_store_unlock(void)
{
    if(store_mutex != NULL) {
        xSemaphoreGiveRecursive(store_mutex);
    }
}

size_t image_store_count(void)
{
    image_store_lock();
    size_t count = stored_image_count;
    image_store_unlock();
    return count;
}

const memory_clock_image_t *image_store_get(size_t index)
{
    image_store_lock();
    const memory_clock_image_t *image = index < stored_image_count ? &stored_images[index] : NULL;
    image_store_unlock();
    return image;
}

image_store_status_t image_store_status(void)
{
    image_store_lock();
    image_store_status_t status = stored_status;
    image_store_unlock();
    return status;
}

void image_store_replace(memory_clock_image_set_t *replacement)
{
    image_store_lock();
    memory_clock_image_t *old_images = stored_images;
    size_t old_count = stored_image_count;

    stored_images = replacement->images;
    stored_image_count = replacement->count;
    stored_status = stored_image_count > 0 ? IMAGE_STORE_HAS_APPOINTMENTS
                                           : IMAGE_STORE_NO_APPOINTMENTS;
    replacement->images = NULL;
    replacement->count = 0;
    image_store_unlock();

    free_images(old_images, old_count);
}

void image_store_mark_server_unavailable_if_empty(void)
{
    image_store_lock();
    if(stored_image_count == 0) {
        stored_status = IMAGE_STORE_SERVER_UNAVAILABLE;
    }
    image_store_unlock();
}

void image_store_free_set(memory_clock_image_set_t *set)
{
    if(set == NULL) return;
    free_images(set->images, set->count);
    set->images = NULL;
    set->count = 0;
}
