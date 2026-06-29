#include "image_store.h"

#include <stdlib.h>

static memory_clock_image_t *stored_images;
static size_t stored_image_count;
static image_store_status_t stored_status = IMAGE_STORE_SERVER_UNAVAILABLE;

static void free_images(memory_clock_image_t *images, size_t count)
{
    if(images == NULL) return;
    for(size_t i = 0; i < count; ++i) {
        free(images[i].bits);
    }
    free(images);
}

size_t image_store_count(void)
{
    return stored_image_count;
}

const memory_clock_image_t *image_store_get(size_t index)
{
    if(index >= stored_image_count) return NULL;
    return &stored_images[index];
}

image_store_status_t image_store_status(void)
{
    return stored_status;
}

void image_store_replace(memory_clock_image_set_t *replacement)
{
    memory_clock_image_t *old_images = stored_images;
    size_t old_count = stored_image_count;

    stored_images = replacement->images;
    stored_image_count = replacement->count;
    stored_status = stored_image_count > 0 ? IMAGE_STORE_HAS_APPOINTMENTS
                                           : IMAGE_STORE_NO_APPOINTMENTS;
    replacement->images = NULL;
    replacement->count = 0;

    free_images(old_images, old_count);
}

void image_store_mark_server_unavailable_if_empty(void)
{
    if(stored_image_count == 0) {
        stored_status = IMAGE_STORE_SERVER_UNAVAILABLE;
    }
}

void image_store_free_set(memory_clock_image_set_t *set)
{
    if(set == NULL) return;
    free_images(set->images, set->count);
    set->images = NULL;
    set->count = 0;
}
