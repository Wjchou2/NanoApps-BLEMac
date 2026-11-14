#include "hb_video_layer.h"

#define LCD ((volatile uint32_t *)0x38900000u)
#define PANEL_STATUS (*(const volatile uint32_t *)0x3830008cu)
#define PANEL_REFRESH 0x080a06c8u
#define DISPLAY_OBJECT (*(const volatile uint32_t *)0x08ae642cu)
#define DISPLAY_VTABLE 0x087a991cu
#define VIDEO_OWNER (*(const volatile uint32_t *)0x08811d74u)
#define LCD_UI_ENABLE 0x10u
#define LCD_VIDEO_ENABLE 0x108u

/* Stock 432 x 240 landscape video window, with the UI window masked. */
static const struct { unsigned offset; uint32_t value; } settings[] = {
    {0x10, 0x321}, {0x14, 0xff},
    {0x118, 1}, {0x11c, 0}, {0x120, 0}, {0x124, 0}, {0x128, 0},
    {0x130, 0}, {0x134, 0x01af00f0}, {0x138, 0}, {0x13c, 0x00f001b0},
    {0x2c0, 0x10000}, {0x2c4, 0xffff}, {0x2e0, 0x03600050}, {0x2e4, 0x03600050},
};
static uint32_t saved[sizeof(settings) / sizeof(settings[0])], saved_enable;
static volatile uint8_t *quiesce;
static bool showing_ui;

static bool refresh_panel(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
    return ((int (*)(unsigned))(PANEL_REFRESH | 1u))(0) == 0;
}

static void restore_windows(void)
{
    LCD[4 / 4] = saved_enable & ~LCD_VIDEO_ENABLE;
    for (unsigned i = 0; i < sizeof(settings) / sizeof(settings[0]); i++)
        LCD[settings[i].offset / 4u] = saved[i];
    LCD[4 / 4] = saved_enable;
}

static volatile uint8_t *stock_refresh_flag(void)
{
    if (*(const volatile uint32_t *)PANEL_REFRESH != 0x41f0e92du ||
        *(const volatile uint32_t *)0x080755b4u != 0x0772f894u)
        return 0;
    uint32_t display = DISPLAY_OBJECT;
    if (display < 0x08000000u || display > 0x0c000000u - 0x800u ||
        (display & 3u) || *(const volatile uint32_t *)(uintptr_t)display != DISPLAY_VTABLE)
        return 0;
    return (volatile uint8_t *)(uintptr_t)(display + 0x772u);
}

bool hb_video_layer_available(void)
{
    volatile uint8_t *flag = stock_refresh_flag();
    return flag && !*flag && !VIDEO_OWNER && !(LCD[4 / 4] & 8u);
}

bool hb_video_layer_idle(void)
{
    return !(PANEL_STATUS & 3u);
}

bool hb_video_layer_present(uint32_t y, uint32_t uv)
{
    if (!y || !uv || !hb_video_layer_idle())
        return false;
    bool starting = !quiesce;
    if (starting) {
        volatile uint8_t *flag = stock_refresh_flag();
        if (!flag || *flag || VIDEO_OWNER || (LCD[4 / 4] & 8u))
            return false;
        *flag = 1;
        __asm__ volatile("dsb sy" ::: "memory");
        if (!hb_video_layer_idle()) {
            *flag = 0;
            return false;
        }
        quiesce = flag;
        saved_enable = LCD[4 / 4];
        for (unsigned i = 0; i < sizeof(settings) / sizeof(settings[0]); i++)
            saved[i] = LCD[settings[i].offset / 4u];
    }
    if (starting || showing_ui) {
        LCD[4 / 4] = saved_enable & ~(LCD_UI_ENABLE | LCD_VIDEO_ENABLE);
        for (unsigned i = 0; i < sizeof(settings) / sizeof(settings[0]); i++)
            LCD[settings[i].offset / 4u] = settings[i].value;
    }
    LCD[0x11c / 4] = y;
    LCD[0x120 / 4] = uv;
    LCD[0x124 / 4] = y + HB_VIDEO_LAYER_WIDTH;
    LCD[0x128 / 4] = uv + HB_VIDEO_LAYER_WIDTH;
    if (starting || showing_ui)
        LCD[4 / 4] = (saved_enable & ~LCD_UI_ENABLE) | LCD_VIDEO_ENABLE;
    showing_ui = false;
    return refresh_panel();
}

bool hb_video_layer_show_ui(void)
{
    if (!quiesce)
        return true;
    if (!hb_video_layer_idle())
        return false;
    if (!showing_ui) {
        restore_windows();
        showing_ui = true;
    }
    return refresh_panel();
}

bool hb_video_layer_close(void)
{
    if (!quiesce)
        return true;
    if (!hb_video_layer_idle())
        return false;
    restore_windows();
    __asm__ volatile("dsb sy" ::: "memory");
    *quiesce = 0;
    quiesce = 0;
    showing_ui = false;
    return true;
}
