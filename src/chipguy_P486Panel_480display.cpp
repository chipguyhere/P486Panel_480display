/*
 * ESP32-P4 DSI Display driver with 480x480 LVGL buffer + PPA scaling to 720x720
 * For 720x720 MIPI DSI display
 */

#ifdef ARDUINO_ESP32P4_DEV

#include "chipguy_P486Panel_480display.h"
#include "esp_lcd_panel_dpi_bb.h"

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_ldo_regulator.h"
#include "driver/gpio.h"
#include "driver/ppa.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_attr.h"
#include "hal/wdt_hal.h"
#include "soc/rtc.h"
#include "hal/lpwdt_ll.h"      // LP_WDT_SWD_WKEY_VALUE
#include "soc/lp_wdt_struct.h"
#include <Preferences.h>

#define MIPI_DSI_PHY_PWR_LDO_CHAN 3
#define MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV 2500
#define MIPI_DPI_PX_FORMAT LCD_COLOR_PIXEL_FORMAT_RGB565
#define NUM_FRAMEBUFFERS 2

#define P486_WIDTH 720
#define P486_HEIGHT 720
#define P486_HSYNC_PULSE_WIDTH 20
#define P486_HSYNC_BACK_PORCH 80
#define P486_HSYNC_FRONT_PORCH 80
#define P486_VSYNC_PULSE_WIDTH 4
#define P486_VSYNC_BACK_PORCH 12
#define P486_VSYNC_FRONT_PORCH 30
#define P486_DPI_CLOCK_HZ 46000000
#define P486_LANE_BIT_RATE_MBPS 600
#define P486_LCD_RST_PIN 27
#define P486_BACKLIGHT_PIN 26

// Recovery reset pulse shape.  See _recoveryResetPulse().
#define P486_RST_LOW_MS     100   // RESETn held low
#define P486_RST_HIGH_MS    100   // RESETn driven high before being released
#define P486_RST_QUIET_MS   100   // pin left undriven, nothing else touched, after the pulse
#define P486_FB_SIZE ((size_t)P486_WIDTH * P486_HEIGHT * sizeof(uint16_t))

#define DRAW_WIDTH 480
#define DRAW_HEIGHT 480
#define DRAW_BUF_SIZE ((size_t)DRAW_WIDTH * DRAW_HEIGHT * sizeof(uint16_t))

static const char *TAG = "P486Panel480";

typedef struct {
    int cmd;
    const void *data;
    size_t data_bytes;
    unsigned int delay_ms;
} chipguy_lcd_init_cmd_t;

// Panel vendor-specific initialization commands
static const chipguy_lcd_init_cmd_t p486_init_cmds[] = {
    {0xB9, (uint8_t[]){0xF1, 0x12, 0x83}, 3, 0},
    {0xBA, (uint8_t[]){0x31, 0x81, 0x05, 0xF9, 0x0E, 0x0E, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x44, 0x25, 0x00, 0x90, 0x0A, 0x00, 0x00, 0x01, 0x4F, 0x01, 0x00, 0x00, 0x37}, 27, 0},
    {0xB8, (uint8_t[]){0x25, 0x22, 0xF0, 0x63}, 4, 0},
    {0xBF, (uint8_t[]){0x02, 0x11, 0x00}, 3, 0},
    {0xB3, (uint8_t[]){0x10, 0x10, 0x28, 0x28, 0x03, 0xFF, 0x00, 0x00, 0x00, 0x00}, 10, 0},
    {0xC0, (uint8_t[]){0x73, 0x73, 0x50, 0x50, 0x00, 0x00, 0x12, 0x70, 0x00}, 9, 0},
    {0xBC, (uint8_t[]){0x46}, 1, 0},
    {0xCC, (uint8_t[]){0x0B}, 1, 0},
    {0xB4, (uint8_t[]){0x80}, 1, 0},
    {0xB2, (uint8_t[]){0x3C, 0x12, 0x30}, 3, 0},
    {0xE3, (uint8_t[]){0x07, 0x07, 0x0B, 0x0B, 0x03, 0x0B, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xC0, 0x10}, 14, 0},
    {0xC1, (uint8_t[]){0x36, 0x00, 0x32, 0x32, 0x77, 0xF1, 0xCC, 0xCC, 0x77, 0x77, 0x33, 0x33}, 12, 0},
    {0xB5, (uint8_t[]){0x0A, 0x0A}, 2, 0},
    {0xB6, (uint8_t[]){0xB2, 0xB2}, 2, 0},
    {0xE9, (uint8_t[]){0xC8, 0x10, 0x0A, 0x10, 0x0F, 0xA1, 0x80, 0x12, 0x31, 0x23, 0x47, 0x86, 0xA1, 0x80, 0x47, 0x08, 0x00, 0x00, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0D, 0x00, 0x00, 0x00, 0x48, 0x02, 0x8B, 0xAF, 0x46, 0x02, 0x88, 0x88, 0x88, 0x88, 0x88, 0x48, 0x13, 0x8B, 0xAF, 0x57, 0x13, 0x88, 0x88, 0x88, 0x88, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 63, 0},
    {0xEA, (uint8_t[]){0x96, 0x12, 0x01, 0x01, 0x01, 0x78, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4F, 0x31, 0x8B, 0xA8, 0x31, 0x75, 0x88, 0x88, 0x88, 0x88, 0x88, 0x4F, 0x20, 0x8B, 0xA8, 0x20, 0x64, 0x88, 0x88, 0x88, 0x88, 0x88, 0x23, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0xA1, 0x80, 0x00, 0x00, 0x00, 0x00}, 61, 0},
    {0xE0, (uint8_t[]){0x00, 0x0A, 0x0F, 0x29, 0x3B, 0x3F, 0x42, 0x39, 0x06, 0x0D, 0x10, 0x13, 0x15, 0x14, 0x15, 0x10, 0x17, 0x00, 0x0A, 0x0F, 0x29, 0x3B, 0x3F, 0x42, 0x39, 0x06, 0x0D, 0x10, 0x13, 0x15, 0x14, 0x15, 0x10, 0x17}, 34, 0},
    {0x11, (uint8_t[]){0x00}, 1, 250},
    {0x29, (uint8_t[]){0x00}, 1, 50},
};


chipguy_P486Panel_480display::chipguy_P486Panel_480display()
{
}

// ---------------------------------------------------------------------------
// Warm-boot reset promotion
//
// The panel intermittently comes up black -- backlight on, firmware running,
// LVGL rendering, the DPI engine scanning real pixels out at full rate, and the
// panel reporting sleep-out and display-on.  Every host-side measurement reads
// identical to a working boot.  Only the picture is missing.
//
// Measured on natural failures (not induced):
//   - light sleep with TOP+CNNT powered down, plus a full DSI rebuild:  no
//   - deep sleep, which powers the entire digital core and PHY down:    no
//   - RWDT reset (rst:0x10 SYS_RWDT):     recovered ONCE, then measured
//                                         firing twice on a black field unit
//                                         with the screen staying black
//   - EN button (whole-chip reset):       reliably recovers
//
// The single RWDT success was almost certainly luck -- bring-up is
// probabilistic, and the same reset later failed twice on the same fault. So
// 0x10 is not deep enough. The Super Watchdog (rst:0x12, documented as
// resetting "the digital core and rtc module") is the deepest cause reachable
// from software and sits between the reset that fails and the EN reset that
// works; it is verified to fire in ~1.8 s and boot cleanly, but is NOT yet
// verified to clear an actual black screen.
//
// So: on a soft reset, bring the display up, shut the DSI down cleanly, then
// promote to a Super Watchdog reset.  Costs one extra ~2 s boot cycle per OTA
// or watchdog reboot, and nothing at all on a cold boot.
//
//
// LOOP SAFETY.  This runs long before the caller has any network up, so a loop
// here would be unrecoverable in the field: no OTA, USB and a ladder only.  Two
// independent guards:
//
//   1. Reset reason.  Our promotion comes back as ESP_RST_WDT (verified for
//      both 0x10 and 0x12), which is deliberately absent from the promote
//      list below.
//   2. An NVS flag.  Reason alone is not enough to bet a fleet on -- if a chip
//      revision ever reported our RWDT reset differently, guard 1 would loop
//      forever.  RTC RAM cannot be used for this, because these deep resets
//      are precisely what wipe it, so the flag lives in NVS.  It is set before
//      promoting and cleared on the next boot, which bounds promotion to at
//      most one per soft reset no matter what the reason reads as.
// ---------------------------------------------------------------------------
// Decide, at the TOP of begin(), whether this boot should end in a promoted
// reset.  The reset itself happens at the *bottom* of begin() -- see the note
// there for why.  This half only reads state and consumes the NVS guard.
bool chipguy_P486Panel_480display::_shouldPromoteWarmBootReset()
{
    esp_reset_reason_t reason = esp_reset_reason();

    // Only the soft-reset family gets promoted.  Everything else -- power-on,
    // EN pin, our own RWDT reset, deep sleep wake -- is either already clean or
    // is the result of a promotion we just did.
    bool promote = (reason == ESP_RST_SW       || reason == ESP_RST_PANIC ||
                    reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT);

    Preferences prefs;
    if (prefs.begin("cgp486", false)) {
        if (prefs.getBool("promoted", false)) {
            // The previous boot promoted.  Consume the flag and proceed
            // normally regardless of what the reset reason says -- this is the
            // guard that holds even if the reason were ever misreported.
            prefs.putBool("promoted", false);
            prefs.end();
            ESP_LOGE(TAG, "warm boot (reason %d): promotion already used this cycle, "
                          "continuing with normal bring-up", (int)reason);
            return false;
        }
        prefs.end();
    } else if (promote) {
        ESP_LOGE(TAG, "NVS unavailable; relying on the reset-reason guard alone");
    }

    if (promote) {
        ESP_LOGE(TAG, "warm boot (reset reason %d): will bring DSI up, shut it down "
                      "cleanly, then promote to an RWDT system+RTC reset", (int)reason);
    }
    return promote;
}

// Deliberately break the DSI link with a DCS read, immediately before the
// promoted reset.
//
// The reasoning: a single DCS read reliably blacks this panel (the Bus Turn
// Around does not resynchronise), and that induced state is reliably cleared by
// a reset -- proven repeatedly on the bench.  The *natural* black state is not
// reliably cleared by a reset.  So if the two are different states, poking the
// link here overwrites whatever the natural latch is with the one failure mode
// we know how to get out of, and the reset that follows lands on a recoverable
// condition instead of an unrecoverable one.
//
// This must go through the async probe rather than calling
// esp_lcd_panel_io_rx_param() directly: the IDF's DSI read busy-waits on the
// host status register with no timeout and no error return, so a panel that
// never answers would hang here forever -- before the caller has any network,
// making the board recoverable only over USB.  startPanelProbe() runs the read
// on a throwaway task and panelProbeState() tears it down on a blown deadline.
void chipguy_P486Panel_480display::_pokeToKnownFailedState()
{
    ESP_LOGE(TAG, "poking the DSI link with a DCS read before the promoted reset");

    startPanelProbe();

    // Bounded wait.  Whether the read completes or stalls is irrelevant -- both
    // outcomes put the link in the state we are aiming for; we only wait so the
    // transaction has actually gone out before we reset.
    uint32_t start = millis();
    while (millis() - start < 800) {
        if (panelProbeState(500) != CG_PANEL_PROBE_RUNNING) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    chipguy_PanelProbeState st = panelProbeState(500);
    ESP_LOGE(TAG, "poke finished, probe state %d (%s)", (int)st,
             st == CG_PANEL_PROBE_TIMEOUT ? "stalled -- panel stopped answering"
                                          : "completed");
}

// Orderly DSI shutdown: give the hardware back in the reverse of the order it
// was taken, with the panel held in reset, so nothing is mid-transaction when
// the reset lands.  Kept for reference/experiments; not on the current path.
void chipguy_P486Panel_480display::_shutdownDsi()
{
    // Wait for any in-flight flush to finish.  Generous timeout, but proceed
    // regardless if it expires -- we are almost always about to reset, and
    // tearing down late beats not tearing down at all.
    bool held = false;
    if (_dsi_mutex) {
        held = (xSemaphoreTake(_dsi_mutex, pdMS_TO_TICKS(500)) == pdTRUE);
        if (!held) {
            ESP_LOGE(TAG, "shutdownDsi: flush still in flight after 500 ms, "
                          "tearing down anyway");
        }
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << P486_LCD_RST_PIN,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);
    gpio_set_level((gpio_num_t)P486_LCD_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(20));

    if (_ppa_client)   { ppa_unregister_client(_ppa_client); _ppa_client = nullptr; }
    if (_panel_handle) { esp_lcd_panel_del(_panel_handle);   _panel_handle = nullptr; }
    if (_io_handle)    { esp_lcd_panel_io_del(_io_handle);   _io_handle = nullptr; }
    if (_mipi_dsi_bus) { esp_lcd_del_dsi_bus(_mipi_dsi_bus); _mipi_dsi_bus = nullptr; }
    if (_ldo_mipi_phy) { esp_ldo_release_channel(_ldo_mipi_phy); _ldo_mipi_phy = nullptr; }
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGE(TAG, "DSI shut down cleanly ahead of promoted reset");

    if (held) xSemaphoreGive(_dsi_mutex);
}

// Fire the RWDT with the RTC domain included.  Does not return in the normal
// case; if the dog never bites we fall through and the caller carries on.
void chipguy_P486Panel_480display::_triggerDeepestReset()
{
    Preferences prefs;
    if (prefs.begin("cgp486", false)) {
        prefs.putBool("promoted", true);
        prefs.end();
    }

    // Super Watchdog, not the RWDT.  P4's reset-cause table:
    //   0x10 SYS_RWDT       "RWDT system reset"
    //   0x12 SYS_SUPER_WDT  "resets the digital core and rtc module"
    // The RWDT reset was measured in the field firing twice on a black unit
    // with the screen staying black, so it is not deep enough.  The SWD is the
    // deepest cause reachable from software and is verified to fire in ~1.8 s
    // and boot cleanly.  Both report as ESP_RST_WDT, so the loop guard is
    // unchanged.
    //
    // The SWD has no timeout register -- it is a fixed hardware timer.  Arm it
    // by clearing swd_disable and swd_auto_feed_en, then simply stop feeding.
    LP_WDT.swd_wprotect.swd_wkey = LP_WDT_SWD_WKEY_VALUE;
    LP_WDT.swd_config.swd_auto_feed_en = 0;
    LP_WDT.swd_config.swd_disable = 0;
    LP_WDT.swd_wprotect.swd_wkey = 0;

    // Bail out rather than hang if the SWD is fused off or refuses to bite --
    // this runs before the caller has OTA, so hanging here needs a cable.
    uint32_t start = millis();
    while (millis() - start < 5000) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGE(TAG, "super watchdog did not fire within 5 s; continuing");
}

// ---------------------------------------------------------------------------
// Panel recovery reset pulse.  Runs at the very top of begin(), before the DSI
// PHY, the bus, or anything else is touched.
//
// Measured in the field 2026-09-03: units that come up black stay black through
// a soft reset AND through an EN-pin reset, but are recovered reliably by
// driving RESETn low for 100 ms, high for 100 ms, and then RELEASING the pin --
// leaving it undriven -- before any initialization is attempted.
//
// The release is the part that is new.  This runs IN ADDITION TO, and ahead of,
// the normal bring-up assert below -- that assert holds RESETn low across the
// DSI bring-up and drives it high afterwards, and is left exactly as it was.
// This pulse only has to happen first, on a panel nothing else has touched yet.
//
// Note the pin is left as a plain input with no internal pull, which is exactly
// what was tested -- RESETn is held by the board's own pull-up.
void chipguy_P486Panel_480display::_recoveryResetPulse()
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << P486_LCD_RST_PIN,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);

    gpio_set_level((gpio_num_t)P486_LCD_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(P486_RST_LOW_MS));
    gpio_set_level((gpio_num_t)P486_LCD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(P486_RST_HIGH_MS));

    // Stop driving.  Hi-Z, not a driven high -- this is the step the recovery
    // depends on.
    io_conf.mode = GPIO_MODE_INPUT;
    gpio_config(&io_conf);

    // Quiet time: pin undriven, nothing else on the panel touched.  The bring-up
    // below re-asserts RESETn almost immediately, so without this the released
    // state would exist for microseconds -- and "released, then left alone" is
    // what was actually observed to recover these panels.
    vTaskDelay(pdMS_TO_TICKS(P486_RST_QUIET_MS));

    ESP_LOGI(TAG, "RESETn recovery pulse: %d ms low, %d ms high, released to input, "
                  "%d ms quiet", P486_RST_LOW_MS, P486_RST_HIGH_MS, P486_RST_QUIET_MS);
}

bool chipguy_P486Panel_480display::begin()
{
    // Recovery reset pulse, before ANY host-side initialization.  Runs ahead of
    // -- not instead of -- the normal bring-up assert further down; see
    // _recoveryResetPulse().
    _recoveryResetPulse();

    // Created before any handle exists, so a flush can never race a teardown
    // even on the very first frame.
    if (!_dsi_mutex) _dsi_mutex = xSemaphoreCreateMutex();

    const chipguy_lcd_init_cmd_t *init_cmds = p486_init_cmds;
    size_t init_cmds_len = sizeof(p486_init_cmds) / sizeof(p486_init_cmds[0]);
    int8_t lcd_rst_pin = P486_LCD_RST_PIN;

    // Assert panel reset (RESETn low) before bringing up the DSI PHY/bus.
    // After a watchdog reset the panel has not been power-cycled and may still
    // be driving / latching state from before. Holding it in reset across the
    // host-side re-init keeps it quiescent while the DSI link comes back up.
    if (lcd_rst_pin >= 0) {
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << lcd_rst_pin,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&io_conf);
        gpio_set_level((gpio_num_t)lcd_rst_pin, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    // ---- SUPER WATCHDOG RESET PROMOTION: RE-ENABLED 2026-09-03 -------------
    // Briefly disabled to test whether the RESETn recovery pulse at the top of
    // begin() recovered every black panel on its own.  It did not: some units
    // stayed stuck through the pulse -- suspected to be a different PCB
    // revision than the ones it does recover.  So both mechanisms now run, in
    // order: the pulse first (cheap, no reboot), the promoted reset after, for
    // whatever the pulse does not clear.
    //
    // Decide now whether this boot ends in a promoted reset; the reset itself
    // is deferred to the bottom of begin().
    _promote_pending = _quiesce_on_warm_boot && _shouldPromoteWarmBootReset();

    // Power on MIPI DSI PHY.  The handle is kept in a member, not a local: it
    // is the DSI PHY's supply, and recoverRebuildDsi() needs to be able to drop
    // it to power-cycle the PHY.
    esp_ldo_channel_config_t ldo_mipi_phy_config = {
        .chan_id = MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &_ldo_mipi_phy));
    ESP_LOGI(TAG, "MIPI DSI PHY Powered on");

    // Create MIPI DSI bus
    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = MIPI_DSI_PHY_PLLREF_CLK_SRC_PLL_F20M,
        .lane_bit_rate_mbps = (float)P486_LANE_BIT_RATE_MBPS,
    };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &_mipi_dsi_bus));
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus = _mipi_dsi_bus;

    // Install MIPI DSI LCD control panel IO
    ESP_LOGI(TAG, "Install MIPI DSI LCD control panel");
    esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &_io_handle));

    // Create DPI panel with double-buffering (still 720x720)
    ESP_LOGI(TAG, "Creating panel %dx%d with %d framebuffers", P486_WIDTH, P486_HEIGHT, NUM_FRAMEBUFFERS);

    chipguy_lcd_dpi_panel_config_t dpi_config = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = P486_DPI_CLOCK_HZ / 1000000,
        .pixel_format = MIPI_DPI_PX_FORMAT,
        .in_color_format = (lcd_color_format_t)0,
        .out_color_format = (lcd_color_format_t)0,
        .num_fbs = NUM_FRAMEBUFFERS,
        .video_timing = {
            .h_size = P486_WIDTH,
            .v_size = P486_HEIGHT,
            .hsync_pulse_width = P486_HSYNC_PULSE_WIDTH,
            .hsync_back_porch = P486_HSYNC_BACK_PORCH,
            .hsync_front_porch = P486_HSYNC_FRONT_PORCH,
            .vsync_pulse_width = P486_VSYNC_PULSE_WIDTH,
            .vsync_back_porch = P486_VSYNC_BACK_PORCH,
            .vsync_front_porch = P486_VSYNC_FRONT_PORCH,
        },
        .virtual_v_pixels = 0,
        .flags = {
            .use_dma2d = false,
            .disable_lp = false,
        },
    };

    ESP_ERROR_CHECK(chipguy_lcd_new_panel_dpi(mipi_dsi_bus, &dpi_config, &_panel_handle));

    // Release panel reset now that the DSI link is up; wait the ST7703's
    // post-reset settling time (>=120 ms) before sending init commands.
    if (lcd_rst_pin >= 0) {
        gpio_set_level((gpio_num_t)lcd_rst_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(120));
        ESP_LOGI(TAG, "Hardware reset complete");
    }

    // Send init commands
    (void)init_cmds; (void)init_cmds_len;
    _sendInitCommands();

    // Initialize panel (starts DMA and video mode)
    ESP_LOGI(TAG, "Initializing panel...");
    esp_err_t ret = esp_lcd_panel_init(_panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Panel init failed: %s", esp_err_to_name(ret));
        return false;
    }

    ESP_LOGI(TAG, "Panel initialized, fb0=%p, fb1=%p, size=%u bytes",
             getFramebuffer(0), getFramebuffer(1), P486_FB_SIZE);

    // Clear both 720x720 framebuffers to black
    uint16_t *fb0 = getFramebuffer(0);
    uint16_t *fb1 = getFramebuffer(1);
    if (fb0) {
        memset(fb0, 0, P486_FB_SIZE);
        esp_cache_msync(fb0, P486_FB_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }
    if (fb1) {
        memset(fb1, 0, P486_FB_SIZE);
        esp_cache_msync(fb1, P486_FB_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }

    // Allocate two 480x480 draw buffers in PSRAM with cache-line alignment.
    // The P4 PSRAM cache line is 128 bytes; aligning to 64 can land the buffer
    // on a non-128-aligned address, which makes the per-frame esp_cache_msync()
    // in scaleAndFlip() log "not aligned with cache line size (0x80)".
    size_t cache_line_size = 128;
    for (int i = 0; i < 2; i++) {
        _draw_buffers[i] = (uint16_t *)heap_caps_aligned_alloc(cache_line_size, DRAW_BUF_SIZE,
                                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!_draw_buffers[i]) {
            ESP_LOGE(TAG, "Failed to allocate 480x480 draw buffer %d", i);
            return false;
        }
        memset(_draw_buffers[i], 0, DRAW_BUF_SIZE);
        esp_cache_msync(_draw_buffers[i], DRAW_BUF_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        ESP_LOGI(TAG, "480x480 draw buffer %d @ %p", i, _draw_buffers[i]);
    }

    // Create PPA SRM client for scaling
    ppa_client_config_t ppa_cfg = {
        .oper_type = PPA_OPERATION_SRM,
    };
    ret = ppa_register_client(&ppa_cfg, &_ppa_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PPA client registration failed: %s", esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI(TAG, "PPA SRM client registered");

    // Promoted reset, deferred to here on purpose.
    //
    // Doing it at the top of begin() was measured NOT to prevent a black
    // bring-up in the field: at that point the DSI hardware is still running
    // from the previous firmware image with no software owning it, so the reset
    // lands mid-stream.  Here the link has been brought up properly, is owned,
    // and _shutdownDsi() hands it back in reverse order with the panel held in
    // reset -- so nothing is in flight when the RTC domain goes.
    //
    // Costs one extra ~1.5 s boot cycle per soft reset.  The next boot comes
    // back as ESP_RST_WDT with the NVS flag set, so it skips this and brings
    // the display up for real.
    if (_promote_pending) {
        _promote_pending = false;
        // NO POKE HERE.  An earlier revision deliberately broke the link with a
        // DCS read first, on the theory that the induced failure was the
        // recoverable one.  That was tested by hand on a black field unit and
        // did NOT recover it -- so the poke only risked breaking a bring-up
        // that had just succeeded, and then resetting with a reset we have
        // measured to be insufficient.  Removed.
        if (_teardown_before_promoted_reset) {
            _shutdownDsi();
        }
        _triggerDeepestReset();     // normally does not return
        // Only reached if the watchdog never fired.  The DSI is torn down, so
        // don't pretend bring-up succeeded; the caller's error path reboots.
        ESP_LOGE(TAG, "promoted reset did not happen; reporting init failure");
        return false;
    }

    return true;
}

void chipguy_P486Panel_480display::_sendInitCommands()
{
    const size_t n = sizeof(p486_init_cmds) / sizeof(p486_init_cmds[0]);
    ESP_LOGI(TAG, "Sending %u init commands", (unsigned)n);
    for (size_t i = 0; i < n; i++) {
        const chipguy_lcd_init_cmd_t *cmd = &p486_init_cmds[i];
        if (cmd->data_bytes > 0) {
            esp_lcd_panel_io_tx_param(_io_handle, cmd->cmd, cmd->data, cmd->data_bytes);
        } else {
            esp_lcd_panel_io_tx_param(_io_handle, cmd->cmd, NULL, 0);
        }
        if (cmd->delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(cmd->delay_ms));
        }
    }
    ESP_LOGI(TAG, "Init commands sent");
}

bool chipguy_P486Panel_480display::_assertPanelReset(uint32_t hold_ms)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << P486_LCD_RST_PIN,
        .mode = GPIO_MODE_OUTPUT,
    };
    if (gpio_config(&io_conf) != ESP_OK) return false;
    gpio_set_level((gpio_num_t)P486_LCD_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(hold_ms));
    gpio_set_level((gpio_num_t)P486_LCD_RST_PIN, 1);
    // ST7703 wants >=120 ms after RESETn release before it will take commands.
    vTaskDelay(pdMS_TO_TICKS(120));
    return true;
}

// ---- Recovery ladder, least invasive first.  See the header. ----

bool chipguy_P486Panel_480display::recoverDisplayOnOff()
{
    if (!_io_handle) return false;
    ESP_LOGW(TAG, "recover 1: DCS display off/on");
    esp_lcd_panel_io_tx_param(_io_handle, 0x28, NULL, 0);   // display off
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_lcd_panel_io_tx_param(_io_handle, 0x29, NULL, 0);   // display on
    vTaskDelay(pdMS_TO_TICKS(50));
    return true;
}

bool chipguy_P486Panel_480display::recoverSleepCycle()
{
    if (!_io_handle) return false;
    ESP_LOGW(TAG, "recover 2: DCS sleep in/out cycle");
    esp_lcd_panel_io_tx_param(_io_handle, 0x28, NULL, 0);   // display off
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_lcd_panel_io_tx_param(_io_handle, 0x10, NULL, 0);   // sleep in
    // Spec minimum between sleep in and sleep out is 120 ms; give it more.
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_lcd_panel_io_tx_param(_io_handle, 0x11, NULL, 0);   // sleep out
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_lcd_panel_io_tx_param(_io_handle, 0x29, NULL, 0);   // display on
    vTaskDelay(pdMS_TO_TICKS(50));
    return true;
}

bool chipguy_P486Panel_480display::recoverPanelReset()
{
    if (!_io_handle) return false;
    ESP_LOGW(TAG, "recover 3: long RESETn assert + re-send init sequence");
    // 200 ms rather than begin()'s 20 ms.  If the panel latches a bad state
    // that a short pulse does not clear, this is where that shows up.
    if (!_assertPanelReset(200)) return false;
    _sendInitCommands();
    return true;
}

bool chipguy_P486Panel_480display::recoverRebuildDsi()
{
    ESP_LOGW(TAG, "recover 4: full DSI teardown and rebuild (incl. PHY LDO)");

    // Hold the panel in reset for the whole rebuild so it is not being fed a
    // half-configured link while the PHY comes back up.
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << P486_LCD_RST_PIN,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);
    gpio_set_level((gpio_num_t)P486_LCD_RST_PIN, 0);

    // Tear down in reverse order of construction.
    if (_panel_handle) { esp_lcd_panel_del(_panel_handle); _panel_handle = nullptr; }
    if (_io_handle)    { esp_lcd_panel_io_del(_io_handle); _io_handle = nullptr; }
    if (_mipi_dsi_bus) { esp_lcd_del_dsi_bus(_mipi_dsi_bus); _mipi_dsi_bus = nullptr; }

    // Drop the PHY supply and let it actually discharge.  This is the step that
    // has no equivalent anywhere in the normal boot path, and the reason this
    // rung exists: esp_restart() leaves this LDO enabled, an EN-pin reset does
    // not.  If the fault is the PHY holding state across a warm reboot, this is
    // what should clear it.
    if (_ldo_mipi_phy) {
        esp_ldo_release_channel(_ldo_mipi_phy);
        _ldo_mipi_phy = nullptr;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    esp_err_t ret = esp_ldo_acquire_channel(&ldo_cfg, &_ldo_mipi_phy);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "recover 4: LDO re-acquire failed: %s", esp_err_to_name(ret));
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = MIPI_DSI_PHY_PLLREF_CLK_SRC_PLL_F20M,
        .lane_bit_rate_mbps = (float)P486_LANE_BIT_RATE_MBPS,
    };
    ret = esp_lcd_new_dsi_bus(&bus_config, &_mipi_dsi_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "recover 4: DSI bus rebuild failed: %s", esp_err_to_name(ret));
        return false;
    }

    esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ret = esp_lcd_new_panel_io_dbi(_mipi_dsi_bus, &dbi_config, &_io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "recover 4: DBI IO rebuild failed: %s", esp_err_to_name(ret));
        return false;
    }

    chipguy_lcd_dpi_panel_config_t dpi_config = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = P486_DPI_CLOCK_HZ / 1000000,
        .pixel_format = MIPI_DPI_PX_FORMAT,
        .in_color_format = (lcd_color_format_t)0,
        .out_color_format = (lcd_color_format_t)0,
        .num_fbs = NUM_FRAMEBUFFERS,
        .video_timing = {
            .h_size = P486_WIDTH,
            .v_size = P486_HEIGHT,
            .hsync_pulse_width = P486_HSYNC_PULSE_WIDTH,
            .hsync_back_porch = P486_HSYNC_BACK_PORCH,
            .hsync_front_porch = P486_HSYNC_FRONT_PORCH,
            .vsync_pulse_width = P486_VSYNC_PULSE_WIDTH,
            .vsync_back_porch = P486_VSYNC_BACK_PORCH,
            .vsync_front_porch = P486_VSYNC_FRONT_PORCH,
        },
        .virtual_v_pixels = 0,
        .flags = {
            .use_dma2d = false,
            .disable_lp = false,
        },
    };
    ret = chipguy_lcd_new_panel_dpi(_mipi_dsi_bus, &dpi_config, &_panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "recover 4: DPI panel rebuild failed: %s", esp_err_to_name(ret));
        return false;
    }

    // Release the panel and give it its post-reset settling time, then re-init.
    gpio_set_level((gpio_num_t)P486_LCD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    _sendInitCommands();

    ret = esp_lcd_panel_init(_panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "recover 4: panel init failed: %s", esp_err_to_name(ret));
        return false;
    }

    // The framebuffers are new allocations owned by the new panel.  Nothing
    // caches them -- scaleAndFlip() calls getFramebuffer() every time -- so the
    // next LVGL flush lands in the right place on its own.  Clear them so a
    // failed recovery reads as black rather than as whatever the new
    // allocation happened to contain.
    for (uint8_t i = 0; i < NUM_FRAMEBUFFERS; i++) {
        uint16_t *fb = getFramebuffer(i);
        if (fb) {
            memset(fb, 0, P486_FB_SIZE);
            esp_cache_msync(fb, P486_FB_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        }
    }
    _active_fb_index = 0xFF;

    ESP_LOGW(TAG, "recover 4: rebuild complete");
    return true;
}

// The six DCS status registers the probe reads, in order.  0x0A is first
// deliberately: it is the one that answers the question (sleep-out + display-on),
// so if the panel only manages one reply we want it to be that one.
static const uint8_t k_probe_regs[6] = {0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};

void chipguy_P486Panel_480display::_probeTaskEntry(void *arg)
{
    auto *self = (chipguy_P486Panel_480display *)arg;
    self->_runProbe();
    // Only reached if every read came back.  A stalled probe never gets here;
    // panelProbeState() deletes the task instead.
    self->_probe_task = nullptr;
    vTaskDelete(nullptr);
}

void chipguy_P486Panel_480display::_runProbe()
{
    chipguy_PanelProbeRegs regs = {};
    // The six register fields are uint8_t and therefore contiguous; index them
    // so the read loop stays a loop rather than six copy-pasted blocks.
    static_assert(offsetof(chipguy_PanelProbeRegs, selfDiag) ==
                  offsetof(chipguy_PanelProbeRegs, powerMode) + 5,
                  "chipguy_PanelProbeRegs register fields must stay contiguous");
    uint8_t *slot = &regs.powerMode;

    for (uint8_t i = 0; i < 6; i++) {
        uint8_t v = 0;
        // NOTE: this call can never return if the panel is wedged.  That is the
        // whole design constraint -- see the header.
        esp_err_t err = esp_lcd_panel_io_rx_param(_io_handle, k_probe_regs[i], &v, 1);
        if (err != ESP_OK) {
            _probe_err = err;
            _probe_regs = regs;
            _probe_phase = 3;
            return;
        }
        slot[i] = v;
        // Publish after every read, so that if the *next* one stalls the caller
        // can still see how far we got and which register hung.
        regs.count = i + 1;
        _probe_regs = regs;
    }

    // Functional check: does the panel's scanline counter actually move?
    // Every register above is configuration readback and reads identical on a
    // working panel and a black one, so this is the only measurement in the
    // probe that can tell them apart -- a panel displaying received video has a
    // moving scanline, one that is not receiving video does not.
    //
    // 7 ms apart is ~0.47 of a frame at 67 fps, deliberately not a whole frame,
    // so a live counter cannot alias back to the same value.
    uint8_t sl[2] = {0, 0};
    if (esp_lcd_panel_io_rx_param(_io_handle, 0x45, sl, 2) == ESP_OK) {
        regs.scanlineA = ((uint16_t)sl[0] << 8) | sl[1];
        vTaskDelay(pdMS_TO_TICKS(7));
        if (esp_lcd_panel_io_rx_param(_io_handle, 0x45, sl, 2) == ESP_OK) {
            regs.scanlineB = ((uint16_t)sl[0] << 8) | sl[1];
            regs.scanlineRead = true;
        }
    }
    _probe_regs = regs;

    _probe_err = ESP_OK;
    _probe_phase = 2;
}

void chipguy_P486Panel_480display::startPanelProbe()
{
    if (!_io_handle) return;
    if (_probe_phase == 1) return;   // already in flight
    if (_probe_wedged) return;       // a previous probe hung; don't strand another task

    _probe_regs = {};
    _probe_err = ESP_OK;
    _probe_start_ms = millis();
    _probe_phase = 1;

    // Priority 1 matches the Arduino loop task, so a spinning probe round-robins
    // with it rather than starving it outright.  Core 1 keeps DSI register
    // traffic off core 0, where the network task services ArduinoOTA -- the
    // whole point being that OTA stays reachable even if this probe never
    // returns.  3 KB is ample for six register reads.
    if (xTaskCreatePinnedToCore(_probeTaskEntry, "panelprobe", 3072, this, 1,
                                &_probe_task, 1) != pdPASS) {
        _probe_task = nullptr;
        _probe_err = ESP_ERR_NO_MEM;
        _probe_phase = 3;
    }
}

chipguy_PanelProbeState chipguy_P486Panel_480display::panelProbeState(uint32_t deadline_ms)
{
    switch (_probe_phase) {
        case 0: return CG_PANEL_PROBE_IDLE;
        case 2: return CG_PANEL_PROBE_OK;
        case 3: return CG_PANEL_PROBE_ERROR;
        case 4: return CG_PANEL_PROBE_TIMEOUT;   // latched; see below
        default: break;
    }

    if (millis() - _probe_start_ms < deadline_ms) {
        return CG_PANEL_PROBE_RUNNING;
    }

    // Deadline blown: the panel is not answering DCS reads.  Kill the stalled
    // task rather than leave it busy-waiting on the status register forever --
    // an endless priority-1 spin would starve core 1's idle task (tripping the
    // idle watchdog) and permanently cost us its stack.  The task holds no lock
    // we care about: the only thing it could be blocking is further DSI command
    // traffic, and on a panel this far gone that traffic is already dead.
    if (_probe_task) {
        vTaskDelete(_probe_task);
        _probe_task = nullptr;
    }
    _probe_wedged = true;
    _probe_phase = 4;   // latched, so every later poll keeps reporting TIMEOUT
    return CG_PANEL_PROBE_TIMEOUT;
}

void chipguy_P486Panel_480display::resetAndRestart(uint32_t hold_ms)
{
    ESP_LOGE(TAG, "Display init failed; holding panel in reset for %u ms, then restarting",
             (unsigned)hold_ms);

    // Kill the backlight so a failed panel isn't left glowing through the wait.
    setBacklight(0);

    // Drive RESETn low and keep it there.  begin() only asserts reset for 20 ms;
    // a much longer assert gives the panel time to drop whatever state wedged
    // it, which a bare esp_restart() on its own would not do -- a chip restart
    // does not power-cycle the panel.
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << P486_LCD_RST_PIN,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);
    gpio_set_level((gpio_num_t)P486_LCD_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(hold_ms));

    // Leave reset asserted across the restart: the GPIO reverts to its default
    // input state as the chip comes up, and begin() re-asserts it immediately.
    ESP_LOGE(TAG, "Restarting");
    esp_restart();
}

uint16_t *chipguy_P486Panel_480display::getDrawBuffer(uint8_t index)
{
    if (index >= 2) return nullptr;
    return _draw_buffers[index];
}

uint16_t *chipguy_P486Panel_480display::getFramebuffer(uint8_t index)
{
    if (!_panel_handle || index >= NUM_FRAMEBUFFERS) {
        return nullptr;
    }

    void *fb_addrs[NUM_FRAMEBUFFERS] = {nullptr, nullptr};
    esp_err_t ret = chipguy_lcd_dpi_panel_get_frame_buffer(_panel_handle, NUM_FRAMEBUFFERS,
                                                           &fb_addrs[0], &fb_addrs[1]);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get framebuffer: %s", esp_err_to_name(ret));
        return nullptr;
    }
    return (uint16_t *)fb_addrs[index];
}

void chipguy_P486Panel_480display::setActiveFramebuffer(uint8_t index, bool wait_for_vsync)
{
    if (_panel_handle) {
        chipguy_lcd_dpi_panel_set_active_fb(_panel_handle, index, wait_for_vsync);
        _active_fb_index = index;
    }
}

uint32_t chipguy_P486Panel_480display::drawBufferNonBlack(uint8_t index, uint32_t stride)
{
    if (index >= 2 || !_draw_buffers[index] || stride == 0) return 0;
    // CPU-written (LVGL renders straight into these), so no cache maintenance
    // is needed to read them back from the CPU.
    const uint16_t *p = _draw_buffers[index];
    const uint32_t px = DRAW_BUF_SIZE / sizeof(uint16_t);
    uint32_t nonblack = 0;
    for (uint32_t i = 0; i < px; i += stride) {
        if (p[i]) nonblack++;
    }
    return nonblack;
}

uint32_t chipguy_P486Panel_480display::framebufferNonBlack(uint8_t index, uint32_t stride)
{
    uint16_t *fb = getFramebuffer(index);
    if (!fb || stride == 0) return 0;
    // These are written by the PPA over DMA, so the CPU's view can be stale.
    // Invalidate before sampling or we would be reading our own old cache
    // lines and could call a live framebuffer black.
    esp_cache_msync(fb, P486_FB_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    const uint32_t px = P486_FB_SIZE / sizeof(uint16_t);
    uint32_t nonblack = 0;
    for (uint32_t i = 0; i < px; i += stride) {
        if (fb[i]) nonblack++;
    }
    return nonblack;
}

void chipguy_P486Panel_480display::scaleAndFlip(uint8_t draw_buf_index, uint8_t target_fb_index)
{
    // Serialise against _shutdownDsi(), which deletes the panel, IO and bus
    // handles this function is about to dereference.  The restart hook fires
    // from the watchdog or network task while LVGL is flushing from loop(), so
    // without this a teardown can land mid-flush and panic -- and a panic
    // reboots with the DSI link still live, which is the exact condition the
    // hook exists to prevent.
    //
    // A short timeout, and dropping the frame if it expires: a teardown is in
    // progress and this frame is about to be irrelevant anyway.  Never block
    // LVGL for long.
    if (_dsi_mutex && xSemaphoreTake(_dsi_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }

    do {
    if (!_ppa_client || draw_buf_index >= 2) break;
    uint16_t *src = _draw_buffers[draw_buf_index];
    if (!src) break;

    uint16_t *target_fb = getFramebuffer(target_fb_index);
    if (!target_fb) break;

    // Flush CPU cache for the 480x480 source buffer
    esp_cache_msync(src, DRAW_BUF_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    // PPA scale-rotate-mirror: 480x480 -> 720x720
    ppa_srm_oper_config_t srm_config = {
        .in = {
            .buffer = (const void *)src,
            .pic_w = DRAW_WIDTH,
            .pic_h = DRAW_HEIGHT,
            .block_w = DRAW_WIDTH,
            .block_h = DRAW_HEIGHT,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = (void *)target_fb,
            .buffer_size = P486_FB_SIZE,
            .pic_w = P486_WIDTH,
            .pic_h = P486_HEIGHT,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = _rotation,
        .scale_x = 1.5f,  // 480 * 1.5 = 720
        .scale_y = 1.5f,
        .rgb_swap = 0,
        .byte_swap = 0,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    esp_err_t ret = ppa_do_scale_rotate_mirror(_ppa_client, &srm_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PPA scale failed: %s", esp_err_to_name(ret));
        break;
    }

    // Invalidate cache for the target 720x720 buffer (PPA wrote via DMA)
    esp_cache_msync(target_fb, P486_FB_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

    // Switch display to show this framebuffer
    setActiveFramebuffer(target_fb_index, false);
    // Written as an assignment rather than ++: C++20 deprecates increment on a
    // volatile-qualified type, but a simple assignment whose result is
    // discarded is not.  Single incrementer, so no read-modify-write race.
    _flush_count = _flush_count + 1;
    } while (0);

    if (_dsi_mutex) xSemaphoreGive(_dsi_mutex);
}

bool chipguy_P486Panel_480display::waitVsync(uint32_t timeout_ms)
{
    return chipguy_lcd_dpi_panel_wait_vsync(timeout_ms);
}

uint8_t chipguy_P486Panel_480display::getNumFramebuffers()
{
    if (_panel_handle) {
        return chipguy_lcd_dpi_panel_get_num_fbs(_panel_handle);
    }
    return 0;
}

uint32_t chipguy_P486Panel_480display::getFrameCount()
{
    return chipguy_lcd_dpi_panel_get_frame_count();
}

uint32_t chipguy_P486Panel_480display::getUnderrunCount()
{
    return chipguy_lcd_dpi_panel_get_underrun_count();
}

void chipguy_P486Panel_480display::setRotation(int degrees)
{
    switch (degrees) {
        case 90:  _rotation = PPA_SRM_ROTATION_ANGLE_90;  break;
        case 180: _rotation = PPA_SRM_ROTATION_ANGLE_180; break;
        case 270: _rotation = PPA_SRM_ROTATION_ANGLE_270; break;
        default:  _rotation = PPA_SRM_ROTATION_ANGLE_0; degrees = 0; break;
    }
    _rotation_degrees = degrees;
}

int chipguy_P486Panel_480display::getRotation() const
{
    return _rotation_degrees;
}

void chipguy_P486Panel_480display::setBacklight(int percentage)
{
    if (percentage <= 0) {
        analogWrite(P486_BACKLIGHT_PIN, 255);
        return;
    }
    if (percentage > 100) percentage = 100;

    // Active-low: LOW = on, HIGH = off
    // The bottom 40% of PWM range produces no visible light,
    // so map 1-100% across the usable upper 60% (duty 153..0)
    int duty = (100 - percentage) * 153 / 99;
    analogWrite(P486_BACKLIGHT_PIN, duty);
}

#endif // ARDUINO_ESP32P4_DEV
