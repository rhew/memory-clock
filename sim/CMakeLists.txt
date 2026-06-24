cmake_minimum_required(VERSION 3.20)

project(mom-clock-sim LANGUAGES C)

include(FetchContent)

find_package(SDL2 REQUIRED CONFIG)

set(LV_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE STRING "" FORCE)
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_THORVG_INTERNAL ON CACHE BOOL "" FORCE)

FetchContent_Declare(
    lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG v9.2.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(lvgl)

set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
set(ENABLE_CUSTOM_COMPILER_FLAGS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    cjson
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG v1.7.19
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(cjson)

target_link_libraries(lvgl PUBLIC SDL2::SDL2)

add_executable(mom-clock-sim
    ../core/clock_app.c
    ../core/ui.c
    src/config_loader.c
    src/main.c
)

target_compile_features(mom-clock-sim PRIVATE c_std_11)
target_include_directories(mom-clock-sim PRIVATE ../core src "${cjson_SOURCE_DIR}")
target_link_libraries(mom-clock-sim PRIVATE cjson lvgl::lvgl SDL2::SDL2)
