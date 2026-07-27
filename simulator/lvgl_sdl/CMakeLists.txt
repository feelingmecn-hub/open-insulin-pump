cmake_minimum_required(VERSION 3.16)
# 必须启用 C 语言: LVGL 核心是 .c 文件, 若只声明 CXX, CMake 会丢弃所有 .c 源,
# 导致 liblvgl.a 只剩 thorvg/gltf 等 .cpp/.S 对象, 链接时缺核心符号(lv_obj/lv_label...)。
project(openloop_pump_sim C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# ---- 拉取并构建 LVGL 9.5.0 (与固件版本一致) ----
# 工程放在 ASCII 软链路径 (/Users/feelingme/pump_sim) 下构建, 规避含中文路径的潜在问题。
include(FetchContent)
FetchContent_Declare(
    lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG        v9.5.0
)
# 强制让 LVGL 的 CMake 使用我们的 src/lv_conf.h (FORCE 覆盖其默认空 cache 变量)
set(LV_BUILD_CONF_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src CACHE PATH "lv_conf.h dir" FORCE)
FetchContent_MakeAvailable(lvgl)

# ---- 查找 SDL2 ----
find_package(SDL2 REQUIRED)

# ---- 源文件 ----
set(SIM_SRC
    src/main.cpp
    src/mock_hal.cpp
    src/pump_state.cpp
    src/ui_screen.cpp
    src/ui_hal_sim.cpp
    src/lv_font_cn_16.c
    src/lv_font_cn_12.c
)

add_executable(simulator ${SIM_SRC})

# 共享剂量换算单一真源: 固件与模拟器共用同一份 code/esp32_firmware/src/dosing.h, 杜绝算法双份。
# 用 REALPATH 解析本机 ASCII 软链 (pump_sim -> .../simulator), 得到真实仓库根后再定位固件 src。
get_filename_component(_SIM_REAL "${CMAKE_CURRENT_SOURCE_DIR}" REALPATH)
get_filename_component(_PROJ_ROOT "${_SIM_REAL}/../.." REALPATH)

target_include_directories(simulator PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${_PROJ_ROOT}/code/esp32_firmware/src
)

# 链接: LVGL (FetchContent 提供的 lvgl::lvgl) + SDL2
target_link_libraries(simulator PRIVATE lvgl::lvgl SDL2::SDL2)
