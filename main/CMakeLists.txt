set(MEMORY_CLOCK_ENV_FILE "${CMAKE_CURRENT_LIST_DIR}/../.env")
if(NOT EXISTS "${MEMORY_CLOCK_ENV_FILE}")
    message(FATAL_ERROR "Missing ${MEMORY_CLOCK_ENV_FILE}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MEMORY_CLOCK_ENV_FILE}")

file(STRINGS "${MEMORY_CLOCK_ENV_FILE}" MEMORY_CLOCK_ENV_LINES)
foreach(MEMORY_CLOCK_ENV_LINE IN LISTS MEMORY_CLOCK_ENV_LINES)
    string(STRIP "${MEMORY_CLOCK_ENV_LINE}" MEMORY_CLOCK_ENV_LINE)
    if(MEMORY_CLOCK_ENV_LINE STREQUAL "" OR MEMORY_CLOCK_ENV_LINE MATCHES "^#")
        continue()
    endif()
    if(MEMORY_CLOCK_ENV_LINE MATCHES "^([A-Za-z_][A-Za-z0-9_]*)=(.*)$")
        set(MEMORY_CLOCK_ENV_KEY "${CMAKE_MATCH_1}")
        set(MEMORY_CLOCK_ENV_VALUE "${CMAKE_MATCH_2}")
        string(REGEX REPLACE "^\"(.*)\"$" "\\1" MEMORY_CLOCK_ENV_VALUE "${MEMORY_CLOCK_ENV_VALUE}")
        set("${MEMORY_CLOCK_ENV_KEY}" "${MEMORY_CLOCK_ENV_VALUE}")
    endif()
endforeach()

if(NOT DEFINED WIFI_SSID OR NOT DEFINED WIFI_PASSWORD)
    message(FATAL_ERROR "Expected WIFI_SSID and WIFI_PASSWORD in ${MEMORY_CLOCK_ENV_FILE}")
endif()

string(REPLACE "\\" "\\\\" WIFI_SSID_ESCAPED "${WIFI_SSID}")
string(REPLACE "\"" "\\\"" WIFI_SSID_ESCAPED "${WIFI_SSID_ESCAPED}")
string(REPLACE "\\" "\\\\" WIFI_PASSWORD_ESCAPED "${WIFI_PASSWORD}")
string(REPLACE "\"" "\\\"" WIFI_PASSWORD_ESCAPED "${WIFI_PASSWORD_ESCAPED}")

message(STATUS "memory-clock Wi-Fi SSID: ${WIFI_SSID}")

set(MEMORY_CLOCK_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${MEMORY_CLOCK_GENERATED_DIR}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/wifi_env.h.in"
               "${MEMORY_CLOCK_GENERATED_DIR}/wifi_env.h" @ONLY)

file(GLOB MEMORY_CLOCK_IMAGE_FILES "${CMAKE_CURRENT_LIST_DIR}/../images/*.xbm")
list(SORT MEMORY_CLOCK_IMAGE_FILES)

set(MEMORY_CLOCK_IMAGE_ASSETS_H "${MEMORY_CLOCK_GENERATED_DIR}/image_assets.h")
set(MEMORY_CLOCK_IMAGE_INCLUDES "")
set(MEMORY_CLOCK_IMAGE_ENTRIES "")
foreach(MEMORY_CLOCK_IMAGE_FILE IN LISTS MEMORY_CLOCK_IMAGE_FILES)
    get_filename_component(MEMORY_CLOCK_IMAGE_NAME "${MEMORY_CLOCK_IMAGE_FILE}" NAME_WE)
    get_filename_component(MEMORY_CLOCK_IMAGE_FILENAME "${MEMORY_CLOCK_IMAGE_FILE}" NAME)
    set(MEMORY_CLOCK_IMAGE_GENERATED_FILE
        "${MEMORY_CLOCK_GENERATED_DIR}/${MEMORY_CLOCK_IMAGE_FILENAME}")
    if(NOT MEMORY_CLOCK_IMAGE_NAME MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR
            "Image filename '${MEMORY_CLOCK_IMAGE_NAME}' must be a valid C identifier")
    endif()

    file(READ "${MEMORY_CLOCK_IMAGE_FILE}" MEMORY_CLOCK_IMAGE_CONTENTS)
    string(REGEX REPLACE "static[ \t]+unsigned[ \t]+char[ \t]+"
           "static const uint8_t "
           MEMORY_CLOCK_IMAGE_CONTENTS "${MEMORY_CLOCK_IMAGE_CONTENTS}")
    string(REGEX REPLACE "static[ \t]+char[ \t]+"
           "static const uint8_t "
           MEMORY_CLOCK_IMAGE_CONTENTS "${MEMORY_CLOCK_IMAGE_CONTENTS}")
    file(WRITE "${MEMORY_CLOCK_IMAGE_GENERATED_FILE}" "${MEMORY_CLOCK_IMAGE_CONTENTS}")

    string(APPEND MEMORY_CLOCK_IMAGE_INCLUDES
           "#include \"${MEMORY_CLOCK_IMAGE_FILENAME}\"\n")
    string(APPEND MEMORY_CLOCK_IMAGE_ENTRIES
           "    {${MEMORY_CLOCK_IMAGE_NAME}_bits, ${MEMORY_CLOCK_IMAGE_NAME}_width, ${MEMORY_CLOCK_IMAGE_NAME}_height},\n")
endforeach()

if(MEMORY_CLOCK_IMAGE_FILES)
    set(MEMORY_CLOCK_IMAGE_TABLE
"static const memory_clock_image_t memory_clock_images[] = {
${MEMORY_CLOCK_IMAGE_ENTRIES}};

static const size_t memory_clock_image_count =
    sizeof(memory_clock_images) / sizeof(memory_clock_images[0]);
")
else()
    set(MEMORY_CLOCK_IMAGE_TABLE
"static const memory_clock_image_t *memory_clock_images = NULL;
static const size_t memory_clock_image_count = 0;
")
endif()

file(WRITE "${MEMORY_CLOCK_IMAGE_ASSETS_H}"
"#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *bits;
    int width;
    int height;
} memory_clock_image_t;

${MEMORY_CLOCK_IMAGE_INCLUDES}
${MEMORY_CLOCK_IMAGE_TABLE}
")

idf_component_register(
    SRCS
        "banner.c"
        "display_port.c"
        "font_assets.c"
        "font_render.c"
        "main.c"
        "provisioning.c"
    INCLUDE_DIRS "." "${MEMORY_CLOCK_GENERATED_DIR}"
    REQUIRES driver esp_netif esp_timer esp_wifi lwip nvs_flash
)
