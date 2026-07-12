get_filename_component(MEMORY_CLOCK_PROJECT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(NOT DEFINED MEMORY_CLOCK_ENV_FILE)
    set(MEMORY_CLOCK_ENV_FILE "${MEMORY_CLOCK_PROJECT_DIR}/env")
else()
    get_filename_component(MEMORY_CLOCK_ENV_FILE "${MEMORY_CLOCK_ENV_FILE}" ABSOLUTE
                           BASE_DIR "${MEMORY_CLOCK_PROJECT_DIR}")
endif()
if(NOT EXISTS "${MEMORY_CLOCK_ENV_FILE}")
    message(FATAL_ERROR "Missing ${MEMORY_CLOCK_ENV_FILE}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MEMORY_CLOCK_ENV_FILE}")

if(NOT DEFINED MEMORY_CLOCK_FONT_FILE)
    set(MEMORY_CLOCK_FONT_FILE "/usr/share/fonts/truetype/lato/Lato-Regular.ttf")
else()
    get_filename_component(MEMORY_CLOCK_FONT_FILE "${MEMORY_CLOCK_FONT_FILE}" ABSOLUTE
                           BASE_DIR "${MEMORY_CLOCK_PROJECT_DIR}")
endif()
if(NOT EXISTS "${MEMORY_CLOCK_FONT_FILE}")
    message(FATAL_ERROR
        "Missing firmware font source ${MEMORY_CLOCK_FONT_FILE}. Install Lato or set MEMORY_CLOCK_FONT_FILE.")
endif()

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

if(NOT DEFINED WIFI_SSID OR NOT DEFINED WIFI_PASSWORD OR NOT DEFINED CLOCK_SERVER_URL
   OR NOT DEFINED BEARER_TOKEN)
    message(FATAL_ERROR
        "Expected WIFI_SSID, WIFI_PASSWORD, CLOCK_SERVER_URL, and BEARER_TOKEN in ${MEMORY_CLOCK_ENV_FILE}")
endif()

if(NOT DEFINED CLOCK_POLL_INTERVAL_MS)
    set(CLOCK_POLL_INTERVAL_MS 300000)
endif()
if(NOT DEFINED TIME_SERVER)
    set(TIME_SERVER "time.cloudflare.com")
endif()
if(NOT DEFINED TIME_ZONE)
    set(TIME_ZONE "EST5EDT,M3.2.0/2,M11.1.0/2")
endif()
if(NOT DEFINED SNTP_SYNC_TIMEOUT_MS)
    set(SNTP_SYNC_TIMEOUT_MS 15000)
endif()
if(NOT DEFINED WIFI_CONNECT_TIMEOUT_MS)
    set(WIFI_CONNECT_TIMEOUT_MS 30000)
endif()
if(NOT DEFINED BATTERY_LOW_MV)
    set(BATTERY_LOW_MV 3500)
endif()
if(NOT DEFINED BATTERY_CLEAR_MV)
    set(BATTERY_CLEAR_MV 3600)
endif()

foreach(MEMORY_CLOCK_INT_VAR IN ITEMS CLOCK_POLL_INTERVAL_MS SNTP_SYNC_TIMEOUT_MS
                                     WIFI_CONNECT_TIMEOUT_MS BATTERY_LOW_MV BATTERY_CLEAR_MV)
    if(NOT ${MEMORY_CLOCK_INT_VAR} MATCHES "^[0-9]+$")
        message(FATAL_ERROR "${MEMORY_CLOCK_INT_VAR} must be an integer in ${MEMORY_CLOCK_ENV_FILE}")
    endif()
endforeach()
if(BATTERY_LOW_MV GREATER_EQUAL BATTERY_CLEAR_MV)
    message(FATAL_ERROR "BATTERY_LOW_MV must be less than BATTERY_CLEAR_MV in ${MEMORY_CLOCK_ENV_FILE}")
endif()

find_package(Git QUIET)
set(MEMORY_CLOCK_VERSION_SHA "unknown")
set(MEMORY_CLOCK_VERSION_DATE "unknown")
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${MEMORY_CLOCK_PROJECT_DIR}"
        OUTPUT_VARIABLE MEMORY_CLOCK_VERSION_SHA
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env TZ=UTC
                "${GIT_EXECUTABLE}" show -s --date=format-local:%Y-%m-%dT%H:%M:%SZ --format=%cd HEAD
        WORKING_DIRECTORY "${MEMORY_CLOCK_PROJECT_DIR}"
        OUTPUT_VARIABLE MEMORY_CLOCK_VERSION_DATE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()
if(MEMORY_CLOCK_VERSION_SHA STREQUAL "")
    set(MEMORY_CLOCK_VERSION_SHA "unknown")
endif()
if(MEMORY_CLOCK_VERSION_DATE STREQUAL "")
    set(MEMORY_CLOCK_VERSION_DATE "unknown")
endif()
if(MEMORY_CLOCK_VERSION_SHA STREQUAL "unknown")
    set(MEMORY_CLOCK_VERSION "unknown")
elseif(MEMORY_CLOCK_VERSION_DATE STREQUAL "unknown")
    set(MEMORY_CLOCK_VERSION "${MEMORY_CLOCK_VERSION_SHA}")
else()
    set(MEMORY_CLOCK_VERSION
        "${MEMORY_CLOCK_VERSION_SHA}-${MEMORY_CLOCK_VERSION_DATE}")
endif()

string(REPLACE "\\" "\\\\" WIFI_SSID_ESCAPED "${WIFI_SSID}")
string(REPLACE "\"" "\\\"" WIFI_SSID_ESCAPED "${WIFI_SSID_ESCAPED}")
string(REPLACE "\\" "\\\\" WIFI_PASSWORD_ESCAPED "${WIFI_PASSWORD}")
string(REPLACE "\"" "\\\"" WIFI_PASSWORD_ESCAPED "${WIFI_PASSWORD_ESCAPED}")
string(REPLACE "\\" "\\\\" CLOCK_SERVER_URL_ESCAPED "${CLOCK_SERVER_URL}")
string(REPLACE "\"" "\\\"" CLOCK_SERVER_URL_ESCAPED "${CLOCK_SERVER_URL_ESCAPED}")
string(REPLACE "\\" "\\\\" BEARER_TOKEN_ESCAPED "${BEARER_TOKEN}")
string(REPLACE "\"" "\\\"" BEARER_TOKEN_ESCAPED "${BEARER_TOKEN_ESCAPED}")
string(REPLACE "\\" "\\\\" TIME_SERVER_ESCAPED "${TIME_SERVER}")
string(REPLACE "\"" "\\\"" TIME_SERVER_ESCAPED "${TIME_SERVER_ESCAPED}")
string(REPLACE "\\" "\\\\" TIME_ZONE_ESCAPED "${TIME_ZONE}")
string(REPLACE "\"" "\\\"" TIME_ZONE_ESCAPED "${TIME_ZONE_ESCAPED}")
string(REPLACE "\\" "\\\\" MEMORY_CLOCK_VERSION_ESCAPED "${MEMORY_CLOCK_VERSION}")
string(REPLACE "\"" "\\\"" MEMORY_CLOCK_VERSION_ESCAPED "${MEMORY_CLOCK_VERSION_ESCAPED}")

message(STATUS "memory-clock Wi-Fi SSID: ${WIFI_SSID}")
message(STATUS "memory-clock server URL: ${CLOCK_SERVER_URL}")
message(STATUS "memory-clock time server: ${TIME_SERVER}")
message(STATUS "memory-clock time zone: ${TIME_ZONE}")
message(STATUS "memory-clock version: ${MEMORY_CLOCK_VERSION}")
message(STATUS "memory-clock font source: ${MEMORY_CLOCK_FONT_FILE}")

set(MEMORY_CLOCK_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${MEMORY_CLOCK_GENERATED_DIR}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/wifi_env.h.in"
               "${MEMORY_CLOCK_GENERATED_DIR}/wifi_env.h" @ONLY)

idf_component_register(
    SRCS
        "banner.c"
        "clock_client.c"
        "display_port.c"
        "font_assets.c"
        "font_render.c"
        "image_store.c"
        "main.c"
        "provisioning.c"
        "status.c"
    INCLUDE_DIRS "." "${MEMORY_CLOCK_GENERATED_DIR}"
    REQUIRES driver esp_adc esp_http_client esp_netif esp_timer esp_wifi json lwip mbedtls nvs_flash
)
