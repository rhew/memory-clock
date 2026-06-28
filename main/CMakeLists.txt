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
