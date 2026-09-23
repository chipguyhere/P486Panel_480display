/*
 * ESP32-P4 DSI Display driver with 480x480 LVGL buffer + PPA scaling to 720x720
 * For 720x720 MIPI DSI display
 */

#pragma once

#ifdef ARDUINO_ESP32P4_DEV

// This display driver allocates its framebuffers in PSRAM.  The board carries
// 32 MB of PSRAM (and 32 MB of flash), so there is ample room -- but it must
// be switched on: the Arduino IDE defines BOARD_HAS_PSRAM when PSRAM is
// enabled (Tools > PSRAM: "Enabled").
// Fail loudly at compile time rather than crashing at runtime if it's off.
#ifndef BOARD_HAS_PSRAM
#error "chipguy_P486Panel_480display requires PSRAM. Enable it via Tools > PSRAM: \"Enabled\" in the Arduino IDE."
#endif

// The Flash Size check that used to live here now sits in
// lv_setup_P486Panel_480.hpp, next to the include guard.  It belongs there because
// this header is compiled into the library's own .cpp as well, where a sketch's
// #define cannot reach it - putting the check in a header only ever included BY a
// sketch is what lets CHIPGUY_ALLOW_ANY_FLASH_SIZE be set the obvious way.

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"   // TaskHandle_t, for the async panel probe below
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_ldo_regulator.h"   // the DSI PHY supply, kept so it can be cycled
#include "driver/ppa.h"

// ---------------------------------------------------------------------------
// Panel liveness probe
//
// Reads DCS status registers back from the panel over the DSI link.  This is a
// diagnostic for the "warm reboot comes up black but the firmware is still
// running" bug: it distinguishes a panel that accepted its init sequence from
// one that is ignoring the host entirely.
//
// It is asynchronous, and that is not a stylistic choice.  The IDF's DSI read
// path (mipi_dsi_hal_host_gen_read_dcs_command -> ..._gen_read_short_packet)
// busy-waits on the host's cmd_pkt_status register with *no timeout, no
// iteration cap and no error return* -- the function returns void.  A panel
// that never answers therefore hangs the calling task forever, and a panel that
// never answers is exactly what this probe exists to find.  So the read runs on
// a throwaway task and the caller polls for completion against a deadline.
//
// The timeout is itself the finding: "the read did not come back" is a positive
// detection of a wedged panel, and needs no interpretation of register values.
// ---------------------------------------------------------------------------
enum chipguy_PanelProbeState : uint8_t {
    CG_PANEL_PROBE_IDLE = 0,   // none started
    CG_PANEL_PROBE_RUNNING,    // in flight, still under the deadline
    CG_PANEL_PROBE_OK,         // completed; regs are valid
    CG_PANEL_PROBE_TIMEOUT,    // blew the deadline -> panel is not answering DCS
    CG_PANEL_PROBE_ERROR,      // the IO layer refused the read
};

// DCS status registers, per the MIPI DCS spec.  Bit meanings that matter for
// the black-screen case are called out; a panel that took its init sequence
// should report powerMode with both D4 (sleep-out) and D2 (display-on) set.
struct chipguy_PanelProbeRegs {
    uint8_t powerMode;    // 0x0A  D7 booster, D4 sleep-out, D3 normal, D2 display-on
    uint8_t addressMode;  // 0x0B  memory access control (rotation/mirror bits)
    uint8_t pixelFormat;  // 0x0C  should read back the RGB565 selection
    uint8_t displayMode;  // 0x0D
    uint8_t signalMode;   // 0x0E
    uint8_t selfDiag;     // 0x0F  D7 register-load ok, D6 functionality-detect ok
    uint8_t count;        // how many of the six actually came back before a stall

    // 0x45 get_scanline, sampled twice ~7 ms apart (about 0.47 of a frame at
    // 67 fps, chosen so a live counter cannot alias back to the same value).
    //
    // This is the only *functional* measurement in the probe.  Everything above
    // is configuration readback, and configuration reads identical on a working
    // panel and a black one -- verified by diffing a healthy boot against a
    // failed one.  A panel that is genuinely displaying received video has a
    // scanline counter that moves; one that is not receiving video does not.
    uint16_t scanlineA;
    uint16_t scanlineB;
    bool scanlineRead;    // false if the panel refused/ignored 0x45 entirely
};


class chipguy_P486Panel_480display {
public:
    chipguy_P486Panel_480display();

    // Initialize the display hardware
    bool begin();

    // Warm-boot DSI quiesce (on by default; see the long note in the .cpp).
    //
    // A soft reset is a CPU-only reset: the DSI PHY stays configured and
    // clocking and the panel stays mid-video-stream, so begin() would otherwise
    // re-initialise a live link -- which intermittently comes up black while
    // the firmware runs on quite happily.  When enabled, begin() drops the
    // peripheral power domains via a ~200 ms light sleep on any boot that was
    // not a power-on, so the PHY starts from the same state a hardware reset
    // would have left it in.
    //
    // Costs ~200 ms on warm boots only, and is self-limiting: three consecutive
    // warm boots without a successful begin() disable it, so it can never trap
    // a board in a reset loop.  Call before begin() to disable.
    // Runs in addition to the RESETn recovery pulse at the top of begin(), which
    // clears some black panels on its own but not all of them; this is the
    // fallback for the rest.  Disabling it leaves only the pulse.
    void setWarmBootQuiesce(bool enable) { _quiesce_on_warm_boot = enable; }

    // Whether the promoted reset tears the DSI down first.  Under test: see the
    // note by chipguy_onBeforeRestart in the .cpp -- teardown-before-reset is
    // currently suspected of CAUSING the black bring-up.
    void setTeardownBeforePromotedReset(bool enable) { _teardown_before_promoted_reset = enable; }
    bool getTeardownBeforePromotedReset() const { return _teardown_before_promoted_reset; }


    // Recovery path for a failed begin().  Holds the panel in hardware reset
    // for hold_ms -- far longer than the 20 ms assert used during normal
    // bring-up -- to let it fully quiesce, then restarts the ESP32 so the
    // whole init sequence runs again from a clean state.  Does not return.
    [[noreturn]] void resetAndRestart(uint32_t hold_ms = 10000);

    // Get 480x480 draw buffers (LVGL renders here, two for DIRECT mode)
    uint16_t *getDrawBuffer(uint8_t index);

    // Get 720x720 DMA framebuffer by index (0 or 1)
    uint16_t *getFramebuffer(uint8_t index);

    // Set which framebuffer the display hardware shows
    void setActiveFramebuffer(uint8_t index, bool wait_for_vsync = true);

    // PPA scale the given 480x480 draw buffer into specified 720x720 DMA fb, then activate it
    void scaleAndFlip(uint8_t draw_buf_index, uint8_t target_fb_index);

    // Wait for vsync (frame boundary)
    bool waitVsync(uint32_t timeout_ms = 50);

    // Get number of framebuffers
    uint8_t getNumFramebuffers();

    // Logical size (what LVGL sees)
    int16_t width() const { return 480; }
    int16_t height() const { return 480; }
    size_t framebufferSize() const { return (size_t)480 * 480 * sizeof(uint16_t); }

    // Native panel size
    int16_t nativeWidth() const { return 720; }
    int16_t nativeHeight() const { return 720; }

    // Set PPA rotation angle in degrees (0, 90, 180, 270 counter-clockwise)
    // Call before or after begin(); takes effect on next scaleAndFlip().
    void setRotation(int degrees);
    int  getRotation() const;

    // Backlight control (0 = off, 100 = full brightness)
    void setBacklight(int percentage);

    // Debug counters
    uint32_t getFrameCount();
    uint32_t getUnderrunCount();

    // ---- Content tracing --------------------------------------------------
    // For the case where the DSI link is provably fine (DPI frames advancing,
    // panel answering DCS) but the screen is still black.  The pixels are then
    // black because something upstream put black there, and these three answer
    // where in the chain that happened:
    //
    //   flushes == 0            -> LVGL never rendered; the framebuffers are
    //                              still the black memset() from begin()
    //   flushes > 0, draw == 0  -> LVGL rendered, but rendered black
    //   draw > 0, fb == 0       -> LVGL drew content, the PPA scale lost it
    //   draw > 0, fb > 0        -> real pixels are being scanned out, so the
    //                              black is downstream of the framebuffer
    //
    // Number of completed scaleAndFlip() operations.
    uint32_t getFlushCount() const { return _flush_count; }

    // Index most recently handed to setActiveFramebuffer(), or 0xFF if never.
    uint8_t getActiveFramebufferIndex() const { return _active_fb_index; }

    // Coarse content check: count non-black pixels on a strided sample of the
    // buffer.  `stride` is prime so the sample cannot alias with the row pitch
    // and silently walk a single column.  Cheap enough to call every few
    // seconds; returns 0 if the buffer does not exist.
    uint32_t drawBufferNonBlack(uint8_t index, uint32_t stride = 97);
    uint32_t framebufferNonBlack(uint8_t index, uint32_t stride = 97);

    // Kick off a panel liveness probe; see the notes above the enum.  Never
    // blocks.  A no-op if one is already in flight, or if a previous probe
    // wedged (we refuse to strand a second task rather than leak one per call
    // -- internal DRAM is the scarce resource on this board, not PSRAM).
    void startPanelProbe();

    // Poll a probe started by startPanelProbe().  On the first call that finds
    // the deadline blown, this reports CG_PANEL_PROBE_TIMEOUT, tears down the
    // stalled task and latches the wedged state so no further probes are run.
    chipguy_PanelProbeState panelProbeState(uint32_t deadline_ms = 250);

    // Registers from the last probe.  Check `count` -- a stalled probe may have
    // returned some of the six before it hung, and which one it hung on is
    // itself informative.
    const chipguy_PanelProbeRegs &panelProbeRegs() const { return _probe_regs; }

    // esp_err_t from a CG_PANEL_PROBE_ERROR result.
    esp_err_t panelProbeError() const { return _probe_err; }

    // ---- Recovery ladder ---------------------------------------------------
    // For the state where the host is provably scanning pixels out (DPI frames
    // advancing, active framebuffer non-black) and the panel answers DCS reads
    // claiming display-on, yet nothing reaches the glass: the LP command
    // channel works, HS video does not.
    //
    // The steps escalate and are separately invocable on purpose -- the useful
    // result is the *least* invasive one that restores the picture, because
    // that is what the automatic recovery path should then do.  Steps 1 and 2
    // are ordinary panel commands and would be safe to run unconditionally on
    // every boot; steps 3 and 4 are not.
    //
    // All four must be called from the same task that calls scaleAndFlip()
    // (i.e. from loop()), since they tear down state that a concurrent flush
    // would be using.
    bool recoverDisplayOnOff();   // 1: DCS display off / display on
    bool recoverSleepCycle();     // 2: DCS display off, sleep in, sleep out, display on
    bool recoverPanelReset();     // 3: long RESETn assert + resend the init sequence
    bool recoverRebuildDsi();     // 4: tear down and rebuild the DSI stack incl. PHY LDO

private:
    static void _probeTaskEntry(void *arg);
    void _runProbe();
    void _sendInitCommands();
    bool _assertPanelReset(uint32_t hold_ms);
    // Drive RESETn low 100 ms, high 100 ms, release the pin (input, no pull),
    // then 100 ms of quiet -- the sequence measured to recover panels that stay
    // black even through an EN-pin reset.  Called first thing in begin(), before
    // any other hardware is touched.  The normal bring-up assert still runs
    // afterwards, unchanged; this only has to happen first.
    void _recoveryResetPulse();
    bool _shouldPromoteWarmBootReset();
    void _pokeToKnownFailedState();
    void _shutdownDsi();
    void _triggerDeepestReset();

    // Serialises the flush path against _shutdownDsi(); see scaleAndFlip().
    SemaphoreHandle_t _dsi_mutex = nullptr;
    bool _quiesce_on_warm_boot = true;
    // MUST default false.  Measured 2026-08-18: promoted reset WITHOUT a
    // teardown recovered a black panel 20/20 across 20 soft resets; WITH the
    // teardown, 0/11.  Tearing the link down before the reset prevents the
    // recovery the reset would otherwise perform.
    bool _teardown_before_promoted_reset = false;
    bool _promote_pending = false;

    // Held rather than leaked as begin() locals, so recoverRebuildDsi() can
    // release them -- power-cycling the PHY supply is the closest software
    // equivalent we have to what an EN-pin reset does to the DSI block.
    esp_ldo_channel_handle_t _ldo_mipi_phy = nullptr;
    esp_lcd_dsi_bus_handle_t _mipi_dsi_bus = nullptr;

    volatile uint32_t _flush_count = 0;
    volatile uint8_t _active_fb_index = 0xFF;

    volatile uint8_t _probe_phase = 0;   // 0 idle, 1 running, 2 ok, 3 error
    volatile bool _probe_wedged = false;
    uint32_t _probe_start_ms = 0;
    esp_err_t _probe_err = ESP_OK;
    chipguy_PanelProbeRegs _probe_regs = {};
    TaskHandle_t _probe_task = nullptr;

    esp_lcd_panel_handle_t _panel_handle = nullptr;
    esp_lcd_panel_io_handle_t _io_handle = nullptr;
    ppa_client_handle_t _ppa_client = nullptr;
    uint16_t *_draw_buffers[2] = {nullptr, nullptr};  // Two 480x480 PSRAM buffers
    ppa_srm_rotation_angle_t _rotation = PPA_SRM_ROTATION_ANGLE_0;
    int _rotation_degrees = 0;
};

#endif // ARDUINO_ESP32P4_DEV
