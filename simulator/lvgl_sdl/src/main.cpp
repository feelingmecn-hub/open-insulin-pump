/**
 * main.cpp — LVGL SDL2 本地模拟器主循环 (可交互版)
 *
 * 在 PC (Mac/Linux/Windows) 上用 SDL2 窗口渲染 LVGL UI, 复用固件的
 * 纯逻辑 (pump_state / ui_screen / mock_hal), 以最快速度验证 172×320
 * (横屏逻辑 320×172) 状态屏的布局、字体、状态机、报警高亮与按键交互。
 *
 * 显示后端差异:
 *   固件  : lv_display_create(320,172) + RGB565_SWAPPED → SPI ST7789
 *   模拟器: lv_display_create(320,172) + RGB565(原生)   → SDL2 纹理
 *
 * 交互:
 *   - 屏幕底部 4 个可点击按钮 (▲ ▼ OK ESC) 对应真机 UP/DOWN/SET/ESC
 *   - 键盘: ↑/↓ 移动, Enter=OK, Esc=返回; 字母 a/c/s/b/i/p/e/r 快速演示
 *   - 鼠标点击由 SDL pointer 输入设备送入 LVGL, 按钮可点
 */
#include "lvgl.h"
#include "ui_screen.h"
#include "mock_hal.h"
#include "config.h"

#include <SDL2/SDL.h>
#include <cstdint>
#include <cstdlib>
#include <cstdio>

// ---- 离屏截图模式: 设置 SIM_SHOT=1 时, 用 SDL 虚拟驱动跑 LVGL,
//      把渲染出的 320×172 像素存成 BMP 后退出 (无需真实屏幕, 用于预览/CI) ----
static bool     s_headless = false;
static bool     s_dump_raw = false;
static const char *s_shot_path = "/tmp/sim_shot.bmp";

// 把 ARGB8888 像素缓冲写成 32-bit BMP (top-down)
static void write_bmp(const char *path, const uint32_t *px, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "open %s failed\n", path); return; }
    uint32_t filesize = 54 + (uint32_t)(w * h * 4);
    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = (uint8_t)(filesize & 0xFF);  hdr[3] = (uint8_t)((filesize>>8)&0xFF);
    hdr[4] = (uint8_t)((filesize>>16)&0xFF); hdr[5] = (uint8_t)((filesize>>24)&0xFF);
    hdr[10] = 54;                                  // 像素数据偏移
    hdr[14] = 40;                                  // DIB 头大小
    hdr[18] = (uint8_t)(w & 0xFF);  hdr[19] = (uint8_t)((w>>8)&0xFF);
    hdr[20] = (uint8_t)((w>>16)&0xFF); hdr[21] = (uint8_t)((w>>24)&0xFF);
    int32_t hh = -h;                               // 负高度 = top-down
    hdr[22] = (uint8_t)(hh & 0xFF);  hdr[23] = (uint8_t)((hh>>8)&0xFF);
    hdr[24] = (uint8_t)((hh>>16)&0xFF); hdr[25] = (uint8_t)((hh>>24)&0xFF);
    hdr[26] = 1;                                   // planes
    hdr[28] = 32;                                  // bpp
    // compression=0, imagesize 等其余字段为 0 即可
    hdr[34] = (uint8_t)((w*h*4) & 0xFF); hdr[35] = (uint8_t)(((w*h*4)>>8)&0xFF);
    hdr[36] = (uint8_t)(((w*h*4)>>16)&0xFF); hdr[37] = (uint8_t)(((w*h*4)>>24)&0xFF);
    fwrite(hdr, 1, 54, f);
    // 内存中 uint32 为 0xAARRGGBB (小端 = B,G,R,A), 与 BMP 32-bit 字节序一致
    fwrite(px, 4, (size_t)w * h, f);
    fclose(f);
}

// ---- 逻辑显示尺寸 (旋转后, 与固件 g_gfx->width()/height() 一致) ----
#define SIM_W  (LCD_ROTATION % 2 == 1 ? LCD_V_RES : LCD_H_RES)   // 320
#define SIM_H  (LCD_ROTATION % 2 == 1 ? LCD_H_RES : LCD_V_RES)   // 172

// ---- SDL 对象 ----
static SDL_Window   *s_window   = nullptr;
static SDL_Renderer *s_renderer = nullptr;
static SDL_Texture  *s_texture  = nullptr;
static uint32_t     *s_pixels   = nullptr;   // 320×172 ARGB8888 像素缓冲

// ---- LVGL 绘图缓冲 (整屏, 原生 RGB565) ----
static lv_color_t   *s_draw_buf = nullptr;

// ---- 鼠标状态 (喂给 LVGL pointer indev) ----
static int16_t          g_mouse_x = 0;
static int16_t          g_mouse_y = 0;
static lv_indev_state_t g_mouse_state = LV_INDEV_STATE_RELEASED;

// ---- flush 回调: LVGL 缓冲 → SDL 纹理 ----
static void sdl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const int32_t w = area->x2 - area->x1 + 1;
    const int32_t h = area->y2 - area->y1 + 1;
    const uint16_t *px = (const uint16_t *)px_map;   // 显示格式 RGB565 = 2 字节/像素

    // 调试: 把第 0 行原始 16 位值 dump 出来, 用于定位颜色解码问题
    if (s_dump_raw && area->y1 == 0) {
        FILE *rf = fopen("/tmp/sim_raw.bin", "wb");
        if (rf) { fwrite(px, 2, (size_t)w * (size_t)h, rf); fclose(rf); }
    }

    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < w; x++) {
            uint16_t v = px[y * w + x];
            uint8_t r = (uint8_t)(((v >> 11) & 0x1F) << 3);
            uint8_t g = (uint8_t)(((v >> 5)  & 0x3F) << 2);
            uint8_t b = (uint8_t)(( v        & 0x1F) << 3);
            uint32_t argb = 0xFF000000u | ((uint32_t)r << 16)
                                       | ((uint32_t)g << 8)
                                       |  (uint32_t)b;
            s_pixels[(area->y1 + y) * SIM_W + (area->x1 + x)] = argb;
        }
    }

    if (!s_headless) {
        SDL_UpdateTexture(s_texture, nullptr, s_pixels, SIM_W * (int)sizeof(uint32_t));
        SDL_RenderClear(s_renderer);
        SDL_RenderCopy(s_renderer, s_texture, nullptr, nullptr);
        SDL_RenderPresent(s_renderer);
    }

    lv_display_flush_ready(disp);
}

// ---- LVGL pointer 输入设备读取回调 ----
static void mouse_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    data->point.x = g_mouse_x;
    data->point.y = g_mouse_y;
    data->state = g_mouse_state;
}

// ---- SDL 事件处理 ----
static bool s_running = true;

static void handle_events(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            s_running = false;
        } else if (e.type == SDL_MOUSEMOTION) {
            g_mouse_x = (int16_t)e.motion.x;
            g_mouse_y = (int16_t)e.motion.y;
        } else if (e.type == SDL_MOUSEBUTTONDOWN) {
            g_mouse_x = (int16_t)e.button.x;
            g_mouse_y = (int16_t)e.button.y;
            g_mouse_state = LV_INDEV_STATE_PRESSED;
        } else if (e.type == SDL_MOUSEBUTTONUP) {
            g_mouse_state = LV_INDEV_STATE_RELEASED;
        } else if (e.type == SDL_KEYDOWN) {
            switch (e.key.keysym.sym) {
                // 菜单导航 (与屏幕按钮一致)
                case SDLK_UP:    ui_screen_key(KEY_UP);   break;
                case SDLK_DOWN:  ui_screen_key(KEY_DOWN); break;
                case SDLK_RETURN:
                case SDLK_KP_ENTER: ui_screen_key(KEY_SET); break;
                case SDLK_ESCAPE: ui_screen_key(KEY_ESC); break;
                // 快速演示快捷键
                case SDLK_a: mock_event('a'); break;
                case SDLK_c: mock_event('c'); break;
                case SDLK_s: mock_event('s'); break;
                case SDLK_b: mock_event('b'); break;
                case SDLK_i: mock_event('i'); break;
                case SDLK_p: mock_event('p'); break;
                case SDLK_e: mock_event('e'); break;
                case SDLK_r: mock_event('r'); break;
                case SDLK_q: s_running = false; break;
                default: break;
            }
        }
    }
}

int main(void)
{
    // ---------- 离屏截图模式? ----------
    const char *shot = getenv("SIM_SHOT");
    if (shot && shot[0] != '\0') {
        s_headless = true;
        if (shot[0] != '1' && shot[0] != '0') s_shot_path = shot;  // 当成路径
    }
    if (getenv("SIM_RAW")) s_dump_raw = true;

    // ---------- SDL2 初始化 (交互模式才需要窗口) ----------
    if (!s_headless) {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            SDL_Log("SDL_Init failed: %s", SDL_GetError());
            return 1;
        }
        s_window = SDL_CreateWindow(
            "OpenLoop Pump Simulator (320x172)",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            SIM_W, SIM_H, SDL_WINDOW_SHOWN);
        if (!s_window) {
            SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
            return 1;
        }
        s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_ACCELERATED);
        if (!s_renderer) {
            SDL_Log("SDL_CreateRenderer failed: %s", SDL_GetError());
            return 1;
        }
        s_texture = SDL_CreateTexture(
            s_renderer, SDL_PIXELFORMAT_ARGB8888,
            SDL_TEXTUREACCESS_STREAMING, SIM_W, SIM_H);
        if (!s_texture) {
            SDL_Log("SDL_CreateTexture failed: %s", SDL_GetError());
            return 1;
        }
    }
    s_pixels = (uint32_t *)calloc((size_t)SIM_W * SIM_H, sizeof(uint32_t));

    // ---------- LVGL 初始化 ----------
    lv_init();
    s_draw_buf = (lv_color_t *)malloc((size_t)SIM_W * SIM_H * sizeof(lv_color_t));

    lv_display_t *disp = lv_display_create(SIM_W, SIM_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);   // 原生, 无 SPI 交换
    lv_display_set_flush_cb(disp, sdl_flush_cb);
    lv_display_set_buffers(disp, s_draw_buf, nullptr,
                           (size_t)SIM_W * SIM_H * sizeof(lv_color_t),
                           LV_DISPLAY_RENDER_MODE_FULL);

    // 鼠标输入设备 (让屏幕按钮可点击)
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, mouse_read);

    ui_screen_init();   // 创建与固件一致的状态屏 + 底部控制按钮
    mock_init();        // 初始化状态机 + 启动自动演示

    // ---------- 离屏截图模式: 渲染各主要页面后存图退出 ----------
    if (s_headless) {
        struct { int scr; const char *path; bool alarm; int nsel; } shots[] = {
            { UI_HOME,         "/tmp/sim_home.bmp",     false, 0 },
            { UI_MENU,         "/tmp/sim_menu.bmp",     false, 0 },
            { UI_BASAL,        "/tmp/sim_basal.bmp",    false, 2 },
            { UI_BOLUS_MENU,   "/tmp/sim_bolus.bmp",    false, 0 },
            { UI_BOLUS_NORMAL, "/tmp/sim_bolus_n.bmp",  false, 0 },
            { UI_BOLUS_WIZARD, "/tmp/sim_wizard.bmp",   false, 0 },
            { UI_ALARM_LIST,   "/tmp/sim_alarm.bmp",    true,  4 },
            { UI_LOOP,         "/tmp/sim_loop.bmp",     false, 0 },
            { UI_SETTINGS,     "/tmp/sim_settings.bmp", false, 0 },
        };
        int n = (int)(sizeof(shots) / sizeof(shots[0]));
        for (int s = 0; s < n; s++) {
            if (shots[s].alarm) mock_event('a');          // 让对应报警激活
            ui_set_screen(shots[s].scr);
            for (int j = 0; j < shots[s].nsel; j++) ui_screen_key(KEY_DOWN);
            for (int i = 0; i < 30; i++) {
                lv_tick_inc(16);
                mock_tick((uint32_t)((s * 1000 + i) * 16));
                ui_screen_refresh();
                lv_timer_handler();
            }
            write_bmp(shots[s].path, s_pixels, SIM_W, SIM_H);
            printf("SIM_SHOT: wrote %s\n", shots[s].path);
        }
        free(s_pixels);
        free(s_draw_buf);
        return 0;
    }

    // ---------- 主循环 ----------
    uint32_t last = SDL_GetTicks();
    while (s_running) {
        handle_events();

        uint32_t now = SDL_GetTicks();
        lv_tick_inc(now - last);
        last = now;

        mock_tick(now);        // 推进模拟场景
        ui_screen_refresh();   // 从 g_pump_state 刷新 label / 菜单
        lv_timer_handler();    // 渲染 (含按钮点击响应)

        SDL_Delay(10);         // ~100Hz, lv_tick_inc 维护真实时间基准
    }

    // ---------- 清理 ----------
    free(s_pixels);
    free(s_draw_buf);
    if (!s_headless) {
        SDL_DestroyTexture(s_texture);
        SDL_DestroyRenderer(s_renderer);
        SDL_DestroyWindow(s_window);
        SDL_Quit();
    }
    return 0;
}
