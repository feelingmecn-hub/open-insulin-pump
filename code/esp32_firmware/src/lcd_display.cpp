/**
 * lcd_display.cpp — 1.47" ST7789 LCD (Arduino_GFX + LVGL 9.5.0)
 *
 * 移植自 Waveshare 官方 Arduino 示例 (GFX_Library_for_Arduino + LVGL 9.5.0),
 * 渲染全中文状态屏 UI (ui_screen.cpp, 与 PC 模拟器同一份代码)。
 */
#include "lcd_display.h"
#include "pump_state.h"   // g_pump_state 实时状态
#include "pump_types.h"

#include "ui_screen.h"    // 全中文 UI 页面状态机 (与模拟器共用)
#include "ui_hal.h"       // UI 硬件抽象 (固件后端)

#include "esp_heap_caps.h"

// ---- Arduino_GFX 总线与面板 ----
static Arduino_DataBus *g_bus = new Arduino_ESP32SPI(
    PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_SCK, PIN_LCD_MOSI, -1 /* MISO 无 */);

static Arduino_GFX *g_gfx = new Arduino_ST7789(
    g_bus, PIN_LCD_RST, LCD_ROTATION, true /* IPS */,
    LCD_H_RES, LCD_V_RES,
    LCD_X_GAP, LCD_Y_GAP, LCD_X_GAP, LCD_Y_GAP);

static uint8_t *s_lvgl_buf = nullptr;
static uint32_t s_last_tick_ms = 0;

// ---- 背光 LEDC ----
#define BACKLIGHT_LEDC_CH      (0)
#define BACKLIGHT_LEDC_FREQ_HZ (5000)
#define BACKLIGHT_LEDC_BITS    (8)
#define LCD_DEFAULT_BRIGHTNESS (40)   // ≤50, 遵守高温警告

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
}
