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

# 共享剂量换算单一真源: 固件与模拟器共用同一份 code/esp32_firmware/src/dosing.h, 杜绝算法双份。
# 用 REALPATH 解析本机 ASCII 软链 (pump_sim -> .../simulator), 得到真实仓库根后再定位固件 src。
get_filename_component(_SIM_REAL "${CMAKE_CURRENT_SOURCE_DIR}" REALPATH)
get_filename_component(_PROJ_ROOT "${_SIM_REAL}/../.." REALPATH)

# 模拟器(纯 C++/SDL)需编译固件侧共享 .cpp (ui_screen.cpp / basal_history.cpp / ...),
# 这些文件无条件 #include <Arduino.h>。真实 ESP32 Arduino 核心会经 Arduino.h 拉入
# FreeRTOS (freertos/FreeRTOS.h), 主机无 FreeRTOS, 故改用最小 Arduino.h 桩:
#   src/arduino_stub/Arduino.h  —— 仅提供 millis()/delay() 等少量符号, 不拉 FreeRTOS。
# (固件侧重的 Preferences/esp_partition/NimBLE 等在 SIMULATOR 下均被 #ifndef SIMULATOR 守卫排除;
#  LINK 模式所需的 NimBLE 由 sim/src/NimBLEDevice.h 假实现提供。)
set(_ARDUINO_STUB "${CMAKE_CURRENT_SOURCE_DIR}/src/arduino_stub")
message(STATUS "Arduino stub for simulator: ${_ARDUINO_STUB}")

# ---- 联调演示模式 (AAPS 蓝牙动态控制同步演示) ----
# 开启后: 把真实固件命令分发核心 (aaps_dana.cpp) 编入模拟器, 由 link_session 驱动,
# 泵屏幕每帧从 g_pump_state 重绘 -> 与 AAPS 命令严格同步; 并起 TCP 控制通道供 GUI 连接。
option(SIM_LINK_MODE "AAPS link-demo mode (compile firmware command core + control IPC)" OFF)

if(SIM_LINK_MODE)
    # 用真实固件 HAL 后端 (ui_hal_fw) 取代 mock 后端; 不编译 mock 演示数据。
    set(SIM_SRC
        src/main.cpp
        src/pump_state.cpp
        ${_PROJ_ROOT}/code/esp32_firmware/src/ui_screen.cpp
        src/ui_hal_link.cpp
        src/link_session.cpp
        src/link_ipc.cpp
        ${_PROJ_ROOT}/code/esp32_firmware/src/aaps_dana.cpp
        ${_PROJ_ROOT}/code/esp32_firmware/src/rtc_clock.cpp
        ${_PROJ_ROOT}/code/esp32_firmware/src/history_log.cpp
        ${_PROJ_ROOT}/code/esp32_firmware/src/basal_history.cpp
        ${_PROJ_ROOT}/code/esp32_firmware/src/dose_log.cpp
        ${_PROJ_ROOT}/test/host/host_glue.cpp
        src/lv_font_cn_16.c
        src/lv_font_cn_12.c
    )
else()
    set(SIM_SRC
        src/main.cpp
        src/mock_hal.cpp
        src/pump_state.cpp
        ${_PROJ_ROOT}/code/esp32_firmware/src/ui_screen.cpp
        src/ui_hal_sim.cpp
        ${_PROJ_ROOT}/code/esp32_firmware/src/history_log.cpp
        ${_PROJ_ROOT}/code/esp32_firmware/src/basal_history.cpp
        ${_PROJ_ROOT}/code/esp32_firmware/src/dose_log.cpp
        src/lv_font_cn_16.c
        src/lv_font_cn_12.c
    )
endif()

add_executable(simulator ${SIM_SRC})

if(SIM_LINK_MODE)
    target_compile_definitions(simulator PRIVATE
        USE_AAPS_DANA AAPS_DANA_HOST_TEST SIM_LINK_MODE SIMULATOR)
    # host_glue 的 g_pump_state 在此模式下为 extern (由 pump_state.cpp 提供)
    target_include_directories(simulator PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${_PROJ_ROOT}/code/esp32_firmware/src
        ${_PROJ_ROOT}/test/host/inc
        ${_ARDUINO_STUB}
    )
    find_package(Threads REQUIRED)
    target_link_libraries(simulator PRIVATE lvgl::lvgl SDL2::SDL2 Threads::Threads)
else()
    target_compile_definitions(simulator PRIVATE SIMULATOR)
    target_include_directories(simulator PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${_PROJ_ROOT}/code/esp32_firmware/src
        ${_ARDUINO_STUB}
    )
    target_link_libraries(simulator PRIVATE lvgl::lvgl SDL2::SDL2)
endif()

# Windows 下: TCP 控制通道 (link_ipc.cpp) 需要 ws2_32。
# 入口点由 main.cpp 自行提供 (WinMain + main 双入口, 见文件尾部), 规避
# "undefined reference to WinMain"; 不强制 -mconsole, 由 mingw 默认子系统决定。
if(WIN32)
    target_link_libraries(simulator PRIVATE ws2_32)
endif()
