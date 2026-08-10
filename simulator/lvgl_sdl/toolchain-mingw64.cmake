# toolchain-mingw64.cmake
# 交叉编译工具链: macOS / Linux 主机 -> Windows (x86_64) 目标
# 用法:
#   cmake -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=simulator/lvgl_sdl/toolchain-mingw64.cmake \
#     -DSIM_LINK_MODE=ON \
#     -DSDL2_DIR=<SDL2-devel-.../x86_64-w64-mingw32/lib/cmake/SDL2> \
#     simulator/lvgl_sdl
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)
set(CMAKE_C_COMPILER   ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_RC_COMPILER  ${TOOLCHAIN_PREFIX}-windres)

# 只在根路径里查找 Windows 目标平台的库/头文件/包, 不污染主机工具
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
