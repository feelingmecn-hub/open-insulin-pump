# OpenLoop Pump — LVGL SDL 本地模拟器

在 PC (Mac / Linux / Windows) 上用 **SDL2 + LVGL 9.5.0** 跑状态屏 UI，无需任何真实硬件，
即可验证 172×320（横屏逻辑 320×172）**白底中文**界面的布局、字体、状态机显示与报警高亮。

> ⚠️ **本模拟器仅用于 UI / 显示层验证，不代表真实给药行为，严禁用于人体。**

## 它是什么 / 不是什么

- **是**：UI 与状态机显示层的快速验证工具。复用固件的**同一份** `ui_screen.cpp`，
  把显示后端从 SPI ST7789 换成 SDL2 窗口，数据由 `ui_hal_sim.cpp` 后端 + `mock_hal.*`
  桩函数驱动（模拟血糖 / 趋势 / 闭环 / 报警等演示状态）。
- **不是**：固件二进制模拟器（不跑 Arduino/FreeRTOS/BLE 协议栈），也不验证硬件驱动。
  要跑真实固件逻辑，请用 Wokwi 在线模拟器（见下）。

### UI-HAL 抽象层（关键设计）

```
ui_screen.cpp  (唯一界面代码, 模拟器与固件共用)
      │ 调用 ui_hal_* 接口
      ▼
ui_hal.h  (统一接口)
   ├── ui_hal_sim.cpp   ← 本模拟器后端 (mock 数据)
   └── ui_hal_fw.cpp    ← ESP32 固件后端 (接真实模块)
```

修改 `ui_screen.cpp` 一次，模拟器与真机界面同步更新，避免「脱节」。

## 目录结构

```
simulator/lvgl_sdl/
├── CMakeLists.txt          # 构建 (FetchContent 拉 LVGL 9.5.0 + find SDL2)
├── README.md
├── preview/                # 9 页 UI 截图 (PNG, 已提交)
└── src/
    ├── config.h            # 从固件原样复制 (引脚/电机/电池/胰岛素/安全常量)
    ├── pump_types.h        # 从固件原样复制 (状态机/报警码/结构)
    ├── pump_state.h/.cpp   # 从固件原样复制 (状态读写 + crc8)
    ├── lv_conf.h           # 从固件原样复制 (LV_COLOR_DEPTH=16)
    ├── ui_screen.h/.cpp    # ★与固件共用的中文 UI (ui_hal 驱动)
    ├── ui_hal.h            # ★UI 硬件抽象接口 (数据读取 + 动作)
    ├── ui_hal_sim.cpp      # ★模拟器后端 (mock 数据)
    ├── mock_hal.h/.cpp     # 外设桩: 驱动 g_pump_state 演示各种状态/报警
    ├── main.cpp            # SDL2 初始化 + LVGL display + flush 回调 + 主循环
    ├── gen_cn_font.py      # 中文字体生成器 (fontTools 子集 + Pillow 渲染)
    ├── cn_charset.txt      # 子集字符清单 (442 字)
    ├── cn_subset.ttf       # 子集化字体 (Heiti SC face 1)
    ├── lv_font_cn_16.c     # 生成的中文字体 (bpp=4, 16px)
    └── lv_font_cn_12.c     # 生成的中文字体 (bpp=4, 12px)
```

> **字体说明**：`gen_cn_font.py` 输出 LVGL `fmt_txt`，采用 **bpp=4 打包**（标准格式，
> 无需 RLE），442 字：16px≈308KB / 12px≈198KB，体积较 bpp=8 减半，适配 ESP32 4MB Flash。
> 若需增删字符，改 `cn_charset.txt` 后重跑 `python3 gen_cn_font.py`。

## 构建与运行 (macOS)

> ### ⚠️ 重要: 项目路径不能含中文 (macOS 专属坑)
> macOS 文件名使用 Unicode 分解形 (NFD)，而 CMake 的 `file(GLOB_RECURSE)` 在中文路径下
> **会匹配不到任何源文件**。本工程依赖 LVGL 的 CMake 用 GLOB 收集全部核心源，一旦路径含中文，
> `liblvgl.a` 只会编进 thorvg / 显示后端等少数文件，链接时会报 `undefined symbol: _lv_obj_create`
> 等核心符号缺失。
>
> **解法: 用一个纯 ASCII 路径的软链来构建/运行**（不要直接 `cd` 进中文目录）：
> ```bash
> # 建一次即可 (把下面路径换成你本机的中文项目目录的 ASCII 软链目标)
> ln -sfn "/你的路径/闭环胰岛素泵项目/simulator" ~/pump_sim
> # 之后所有 cmake 命令都用 ~/pump_sim 这个 ASCII 路径
> ```

```bash
# 1. 安装依赖
brew install sdl2 cmake ninja

# 2. 建 ASCII 软链 (见上方说明, 只需一次)
ln -sfn "/你的路径/闭环胰岛素泵项目/simulator" ~/pump_sim

# 3. 配置 (FetchContent 自动从 GitHub 拉 LVGL 9.5.0)
cd ~/pump_sim
cmake -S ~/pump_sim/lvgl_sdl -B ~/pump_sim_build \
  -G Ninja \
  -DCMAKE_PREFIX_PATH=/usr/local \
  -DCONFIG_LV_BUILD_DEMOS=OFF \
  -DCONFIG_LV_BUILD_EXAMPLES=OFF \
  -DCONFIG_LV_USE_THORVG_INTERNAL=OFF

# 4. 编译 (首次会编译 LVGL 核心约 600 个文件, 需几分钟)
cmake --build ~/pump_sim_build -j4

# 5. 运行 (自动弹出 320×172 窗口)
~/pump_sim_build/simulator
```

> 离线环境：若 `cmake` 配置阶段无法访问 GitHub，手动克隆 LVGL 到 `simulator/lvgl_sdl/lvgl`
> （`git clone --depth 1 --branch v9.5.0 https://github.com/lvgl/lvgl.git lvgl`），
> 并把 `CMakeLists.txt` 里的 `FetchContent_Declare(lvgl ...)` 改为 `add_subdirectory(lvgl)`。
> 注意: 克隆出的 lvgl 目录同样必须处在 ASCII 路径下。

## 构建与运行 (Linux)

> Linux 路径一般不含中文，无上述 GLOB 问题。若你的目录含中文，同样用 ASCII 软链绕过。

```bash
sudo apt install libsdl2-dev cmake ninja-build build-essential
# 若项目在中文路径下, 先建软链:
#   ln -sfn "/路径/含中文/simulator" ~/pump_sim
#   cd ~/pump_sim
cd simulator/lvgl_sdl
cmake -S . -B build -G Ninja \
  -DCONFIG_LV_BUILD_DEMOS=OFF \
  -DCONFIG_LV_BUILD_EXAMPLES=OFF \
  -DCONFIG_LV_USE_THORVG_INTERNAL=OFF
cmake --build build -j4
./build/simulator
```

## 交互方式 (完全模拟, 可点击)

界面底部有 **4 个可点击按钮 ▲ ▼ OK ESC**，对应真机的 UP / DOWN / SET / ESC 物理键，
鼠标直接点即可。键盘等价映射如下：

### 菜单导航 (按钮点击 / 键盘一致)

| 操作 | 屏幕按钮 | 键盘 | 说明 |
|------|----------|------|------|
| 进入菜单 | 点 `OK` | `Enter` | 从状态屏切到菜单 |
| 上移 / 下移 | 点 `▲` / `▼` | `↑` / `↓` | 在菜单项间移动 |
| 选中执行 | 点 `OK` | `Enter` | 触发当前菜单项 |
| 返回状态屏 | 点 `ESC` | `Esc` | 回到状态屏 |

菜单项（与固件一致）：基础率 / 大剂量 / 排气 / 报警 / 闭环 / 设置。

### 快速演示快捷键 (mock_hal 驱动, 无需进菜单)

| 按键 | 效果 |
|------|------|
| `a`  | 触发 ALARM_PUMP_STALLED（泵卡住报警，状态变 ALARM） |
| `c`  | 清除报警 |
| `s`  | 切换丢步标志（显示红字 `STEP LOSS!`） |
| `b`  | 模拟一次大剂量（状态 BOLUS + IOB 跳变） |
| `l`  | 切换闭环模式 (AAPS 接管 / 开环本地 / 暂停) |
| `i`  | 切到 IDLE |
| `p`  | 切到 PRIMING |
| `e`  | 切到 ERROR |
| `r`  | 恢复自动演示 |
| `q`  | 退出 |

不加任何操作时，模拟器会自动演示：前 2 秒 `BOOT`，之后进入 `BASAL` 常态，
电量缓慢下降、电机周期性微步、储药器递减、IOB 周期跳动。

## ⚠️ 历史备注: 旧调试屏的标签裁切

早期固件 `lcd_display.cpp` 的英文调试屏曾用 8 个状态 label 以 `y = 30 + i*22` 排列，
第 8 个 y=184 超出横屏高度 172 被裁切。该问题已在**改为共用白底中文 `ui_screen.cpp`**
后解决——新界面布局已验证完整落在 172 高度内（见 `preview/` 截图）。

## 与 Wokwi 的对比

| 维度 | 本 SDL 模拟器 | Wokwi 在线 |
|------|---------------|------------|
| 跑的内容 | 纯 UI + 状态机显示 | 真实 Arduino 固件 (含 FreeRTOS/BLE) |
| 外设 | INA226/电机/BLE 全 mock | 部分支持，INA226/电机/BLE 需桩代码 |
| 速度 | 本地编译，秒级 | 浏览器内，需联网 |
| 用途 | 看界面/布局/报警高亮 | 验证固件逻辑/协议/引脚 |

两者互补：用本模拟器快速迭代 UI，用 Wokwi 验证整固件行为。
