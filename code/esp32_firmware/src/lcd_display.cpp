/**
 * lcd_display.cpp — 1.47" ST7789 LCD (Arduino_GFX + LVGL 9.5.0)
 *
 * 移植自 Waveshare 官方 Arduino 示例 (GFX_Library_for_Arduino + LVGL 9.5.0),
 * 渲染全中文状态屏 UI (ui_screen.cpp, 与 PC 模拟器同一份代码)。
 */
#include <Arduino.h>
#include "lcd_display.h"
#include "pump_state.h"   // g_pump_state 实时状态
#include "pump_types.h"

#include "ui_screen.h"    // 全中文 UI 页面状态机 (与模拟器共用)
#include "ui_hal.h"       // UI 硬件抽象 (固件后端)
#include "ina226.h"       // g_ina226_online 电流传感在线标志
#include "rtc_clock.h"    // rtc_is_set / rtc_unix_now / rtc_unix_to_ymdhms

#include "esp_heap_caps.h"

// ---- 开机画面配色 (与 ui_screen.cpp 医疗蓝白底风格一致, 本文件独立副本) ----
#define BOOT_BG      lv_color_hex(0xffffff)
#define BOOT_TITLE   lv_color_hex(0x0a2a43)
#define BOOT_DIM     lv_color_hex(0x8a95a5)
#define BOOT_GREEN   lv_color_hex(0x2e9e4f)
#define BOOT_YELLOW  lv_color_hex(0xc79100)
#define BOOT_RED     lv_color_hex(0xd83a3a)
#define BOOT_FONT    (&lv_font_cn_16)
#define BOOT_FONT_SM (&lv_font_cn_12)

// ---- Arduino_GFX 总线与面板 ----
static Arduino_DataBus *g_bus = new Arduino_ESP32SPI(
    PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_SCK, PIN_LCD_MOSI, -1 /* MISO 无 */);

static Arduino_GFX *g_gfx = new Arduino_ST7789(
    g_bus, PIN_LCD_RST, LCD_ROTATION, true /* IPS */,
    LCD_H_RES, LCD_V_RES,
    LCD_X_GAP, LCD_Y_GAP, LCD_X_GAP, LCD_Y_GAP);

static uint8_t *s_lvgl_buf = nullptr;
static uint32_t s_last_tick_ms = 0;

// ---- 省电: 空闲自动熄屏 ----
static uint32_t s_last_user_activity_ms = 0;   // 最近一次用户/BLE 活动时刻
static bool     s_display_dimmed = false;      // 当前是否已熄屏

// 由 keypad / ble 在用户或手机交互时调用, 标记"有活动"以唤醒/保持屏幕
void ui_hal_mark_activity(void) { s_last_user_activity_ms = millis(); }

// ---- 背光 LEDC ----
#define BACKLIGHT_LEDC_CH      (0)
#define BACKLIGHT_LEDC_FREQ_HZ (5000)
#define BACKLIGHT_LEDC_BITS    (8)
#define LCD_DEFAULT_BRIGHTNESS (10)   // 省电默认 10% (≤50 遵守高温警告)

void lcd_display_backlight(uint8_t percent)
{
    if (percent > 100) percent = 100;
    const uint32_t duty = (percent * ((1u << BACKLIGHT_LEDC_BITS) - 1)) / 100;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(PIN_LCD_BL, duty);
#else
    ledcWrite(BACKLIGHT_LEDC_CH, duty);
#endif
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const int32_t width  = area->x2 - area->x1 + 1;
    const int32_t height = area->y2 - area->y1 + 1;
    g_gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t *)px_map, width, height);
    lv_display_flush_ready(disp);
}

void lcd_display_init(void)
{
    // 背光
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(PIN_LCD_BL, BACKLIGHT_LEDC_FREQ_HZ, BACKLIGHT_LEDC_BITS);
#else
    ledcSetup(BACKLIGHT_LEDC_CH, BACKLIGHT_LEDC_FREQ_HZ, BACKLIGHT_LEDC_BITS);
    ledcAttachPin(PIN_LCD_BL, BACKLIGHT_LEDC_CH);
#endif
    lcd_display_backlight(0);

    if (!g_gfx->begin(LCD_SPI_FREQ_HZ)) {
        Serial.println("LCD init failed");
        return;
    }
    g_gfx->invertDisplay(false);
    g_gfx->fillScreen(0x0000);
    g_gfx->displayOn();
    delay(200);
    lcd_display_backlight(LCD_DEFAULT_BRIGHTNESS);
    s_last_user_activity_ms = millis();   // 启动即视为有活动(避免立即熄屏)

    // LVGL
    lv_init();
    const uint32_t buf_size = (uint32_t)g_gfx->width() * 40 * sizeof(lv_color16_t);
    s_lvgl_buf = (uint8_t *)heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_lvgl_buf) {
        s_lvgl_buf = (uint8_t *)malloc(buf_size);
    }
    if (!s_lvgl_buf) {
        Serial.println("No memory for LVGL draw buffer");
        return;
    }

    lv_display_t *disp = lv_display_create(g_gfx->width(), g_gfx->height());
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);
    lv_display_set_buffers(disp, s_lvgl_buf, NULL, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    s_last_tick_ms = millis();

    // 开机画面 (独立 splash): 显示项目名/版本/自检项, 停留后销毁进入主界面
    lcd_display_boot_screen(2000);

    // 创建与 PC 模拟器完全一致的全中文状态屏 (含底部 4 控制按钮)
    ui_screen_init();
}

void lcd_display_task(void)
{
    const uint32_t now = millis();
    lv_tick_inc(now - s_last_tick_ms);
    s_last_tick_ms = now;

    // 从 g_pump_state / ui_hal 重建当前页面 (与模拟器共用逻辑)
    ui_screen_refresh();

    lv_timer_handler();

    // ---- 省电: 空闲自动熄屏 ----
    // 超时且非关键活动(大剂量/报警)时, 仅关闭背光省电; 面板(ST7789)保持唤醒,
    // 不调 displayOff() —— 否则面板休眠后 SPI/面板状态异常会导致系统复位, 进而 BLE
    // 断连(手机无法随时连接)。任意按键 / 手机指令 / 关键活动都会经
    // ui_hal_mark_activity() 唤醒并恢复背光。
    bool keep_on = ui_hal_bolus_active() || g_pump_state.alarm_active;
    if (g_pump_config.auto_dim_enabled) {
        uint32_t idle_ms = now - s_last_user_activity_ms;
        uint32_t limit_ms = (uint32_t)g_pump_config.auto_dim_timeout_s * 1000UL;
        if (!s_display_dimmed && idle_ms >= limit_ms && !keep_on) {
            s_display_dimmed = true;
            lcd_display_backlight(0);   // 仅关背光(面板仍在线, BLE 不受影响)
        } else if (s_display_dimmed && (idle_ms < limit_ms || keep_on)) {
            s_display_dimmed = false;
            lcd_display_backlight(g_pump_config.display_brightness);
        }
    } else if (s_display_dimmed) {
        // 自动熄屏被关闭: 恢复背光
        s_display_dimmed = false;
        lcd_display_backlight(g_pump_config.display_brightness);
    }
}

// ---- 开机画面 (独立 splash) ----
// 在 lcd_display_init 中、ui_screen_init 之前调用。此时 display_task 尚未启动,
// 故每一步手动 lv_timer_handler() 刷新。仅展示自检结果, 不阻断运行
// (关键项失败时阻断 + 蜂鸣 + 故障码留待后续 POST 完善)。
void lcd_display_boot_screen(int hold_ms)
{
    if (hold_ms <= 0) hold_ms = 2000;

    const int W = g_gfx->width();
    const int H = g_gfx->height();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *boot = lv_obj_create(scr);
    lv_obj_set_size(boot, W, H);
    lv_obj_set_style_bg_color(boot, BOOT_BG, 0);
    lv_obj_set_style_border_width(boot, 0, 0);
    lv_obj_clear_flag(boot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(boot, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *title = lv_label_create(boot);
    lv_obj_set_style_text_font(title, BOOT_FONT, 0);
    lv_obj_set_style_text_color(title, BOOT_TITLE, 0);
    lv_label_set_text(title, "OpenLoop 闭环胰岛素泵");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *ver = lv_label_create(boot);
    lv_obj_set_style_text_font(ver, BOOT_FONT_SM, 0);
    lv_obj_set_style_text_color(ver, BOOT_DIM, 0);
    lv_label_set_text(ver, "固件 v10.2  实验项目");
    lv_obj_align(ver, LV_ALIGN_TOP_MID, 0, 36);

    lv_obj_t *it0 = lv_label_create(boot);
    lv_obj_t *it1 = lv_label_create(boot);
    lv_obj_t *it2 = lv_label_create(boot);
    lv_obj_set_style_text_font(it0, BOOT_FONT_SM, 0);
    lv_obj_set_style_text_font(it1, BOOT_FONT_SM, 0);
    lv_obj_set_style_text_font(it2, BOOT_FONT_SM, 0);
    lv_obj_set_style_text_color(it0, BOOT_DIM, 0);
    lv_obj_set_style_text_color(it1, BOOT_DIM, 0);
    lv_obj_set_style_text_color(it2, BOOT_DIM, 0);
    lv_label_set_text(it0, "配置档案(NVS)  检测中...");
    lv_label_set_text(it1, "系统时钟  检测中...");
    lv_label_set_text(it2, "电流传感(INA226)  检测中...");
    lv_obj_align(it0, LV_ALIGN_TOP_LEFT, 24, 66);
    lv_obj_align(it1, LV_ALIGN_TOP_LEFT, 24, 88);
    lv_obj_align(it2, LV_ALIGN_TOP_LEFT, 24, 110);

    lv_obj_t *warn = lv_label_create(boot);
    lv_obj_set_style_text_font(warn, BOOT_FONT_SM, 0);
    lv_obj_set_style_text_color(warn, BOOT_RED, 0);
    lv_label_set_text(warn, "注意：实验项目 禁止用于人体");
    lv_obj_align(warn, LV_ALIGN_BOTTOM_MID, 0, -26);

    lv_obj_t *foot = lv_label_create(boot);
    lv_obj_set_style_text_font(foot, BOOT_FONT_SM, 0);
    lv_obj_set_style_text_color(foot, BOOT_DIM, 0);
    lv_label_set_text(foot, "启动中...");
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -8);

    lv_timer_handler();   // 首帧刷新 (display_task 尚未启动, 必须手动)
    delay(350);

    // ---- 自检判定 (此时 storage/rtc/ina226 均已初始化) ----
    // 1) 配置档案完整性: 激活方案的 24 段是否合法 (有限 + [0,MAX]) 且非全零
    uint8_t ap = g_pump_config.active_profile;
    if (ap >= MAX_BASAL_PROFILES) ap = 0;
    bool cfg_ok = true, cfg_zero = true;
    for (int i = 0; i < BASAL_SLOTS_PER_DAY; i++) {
        float r = g_pump_config.profiles[ap].slots[i].rate_uh;
        if ((r != r) || r < 0.0f || r > MAX_BASAL_RATE) { cfg_ok = false; break; }
        if (r > 0.0f) cfg_zero = false;
    }
    // 2) 系统时钟: 已设置且年份合理 (≥2024)
    bool clk_ok = rtc_is_set();
    if (clk_ok) {
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
        rtc_unix_to_ymdhms(rtc_unix_now(), &y, &mo, &d, &h, &mi, &s);
        clk_ok = (y >= 2024);
    }
    // 3) 电流传感 INA226 (台架可选, 离线不计入故障)
    bool ina_on = g_ina226_online;

    // ---- 更新显示 ----
    if (cfg_ok && !cfg_zero) {
        lv_label_set_text(it0, "配置档案(NVS)  正常");
        lv_obj_set_style_text_color(it0, BOOT_GREEN, 0);
    } else {
        lv_label_set_text(it0, cfg_ok ? "配置档案(NVS)  全零未设" : "配置档案(NVS)  异常");
        lv_obj_set_style_text_color(it0, BOOT_YELLOW, 0);
    }
    if (clk_ok) {
        lv_label_set_text(it1, "系统时钟  正常");
        lv_obj_set_style_text_color(it1, BOOT_GREEN, 0);
    } else {
        lv_label_set_text(it1, "系统时钟  未设置");
        lv_obj_set_style_text_color(it1, BOOT_YELLOW, 0);
    }
    if (ina_on) {
        lv_label_set_text(it2, "电流传感(INA226)  正常");
        lv_obj_set_style_text_color(it2, BOOT_GREEN, 0);
    } else {
        lv_label_set_text(it2, "电流传感(INA226)  未连接");
        lv_obj_set_style_text_color(it2, BOOT_DIM, 0);
    }

    lv_timer_handler();   // 结果帧刷新
    int remain = hold_ms - 350;
    if (remain > 0) delay((uint32_t)remain);

    lv_obj_del(boot);     // 销毁 splash, 交回 ui_screen_init() 建主界面
}
