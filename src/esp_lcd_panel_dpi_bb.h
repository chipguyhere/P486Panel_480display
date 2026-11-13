/*
 * SPDX-FileCopyrightText: 2024-2026 chipguyhere
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP32-P4 MIPI DSI DPI Panel Driver with page flipping and hardware scrolling.
 * Forked from ESP-IDF esp_lcd_panel_dpi.c; see the .c file for what changed.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Event callbacks for DPI panel (chipguy custom)
 */
typedef struct {
    esp_lcd_dpi_panel_color_trans_done_cb_t on_color_trans_done;  // When color transfer done
    esp_lcd_dpi_panel_refresh_done_cb_t on_refresh_done;          // When frame refresh done
} chipguy_lcd_dpi_panel_event_callbacks_t;

/**
 * @brief Configuration for DPI panel with scrolling support (chipguy custom)
 */
typedef struct {
    uint8_t virtual_channel;
    mipi_dsi_dpi_clock_source_t dpi_clk_src;
    uint32_t dpi_clock_freq_mhz;
    lcd_color_rgb_pixel_format_t pixel_format;
    lcd_color_format_t in_color_format;
    lcd_color_format_t out_color_format;
    uint8_t num_fbs;  // Number of framebuffers
    esp_lcd_video_timing_t video_timing;
    uint32_t virtual_v_pixels;     // Framebuffer height in rows, >= the panel's (0 = same as the panel)
    struct {
        uint32_t use_dma2d: 1;
        uint32_t disable_lp: 1;
    } flags;
} chipguy_lcd_dpi_panel_config_t;

/**
 * @brief Create a new DPI panel with hardware scrolling support
 *
 * @param bus MIPI DSI bus handle
 * @param panel_config Panel configuration
 * @param ret_panel Returned panel handle
 * @return ESP_OK on success
 */
esp_err_t chipguy_lcd_new_panel_dpi(
    esp_lcd_dsi_bus_handle_t bus,
    const chipguy_lcd_dpi_panel_config_t *panel_config,
    esp_lcd_panel_handle_t *ret_panel
);

/**
 * @brief Register event callbacks for DPI panel
 *
 * @param panel Panel handle
 * @param cbs Callback functions
 * @param user_ctx User context passed to callbacks
 * @return ESP_OK on success
 */
esp_err_t chipguy_lcd_dpi_panel_register_event_callbacks(
    esp_lcd_panel_handle_t panel,
    const chipguy_lcd_dpi_panel_event_callbacks_t *cbs,
    void *user_ctx
);

/**
 * @brief Get framebuffer address
 *
 * @param panel Panel handle
 * @param fb_num Number of framebuffers to get
 * @param fb0 Pointer to receive first framebuffer address
 * @param ... Additional framebuffer pointers
 * @return ESP_OK on success
 */
esp_err_t chipguy_lcd_dpi_panel_get_frame_buffer(
    esp_lcd_panel_handle_t panel,
    uint32_t fb_num,
    void **fb0,
    ...
);

/**
 * @brief How each frame is read out of the framebuffer (rows, top to bottom)
 *
 * The panel shows, in order:
 *   1. rows [0, top) of the framebuffer -- a frozen top band;
 *   2. `window` rows taken from a ring of `ring` rows that starts at framebuffer
 *      row `top`, beginning `offset` rows into the ring and wrapping round its
 *      end -- the scrolling area;
 *   3. the remaining (v_pixels - top - window) rows from framebuffer row
 *      top + ring onward -- a frozen bottom band.
 * The plain layout is { 0, v_pixels, v_pixels, 0 }.  A ring longer than the
 * window holds rows that are not on screen, to draw into before they scroll in;
 * the framebuffer must then be taller than the panel (virtual_v_pixels).
 */
typedef struct {
    uint32_t top;
    uint32_t ring;
    uint32_t window;
    uint32_t offset;
} chipguy_lcd_dpi_layout_t;

/**
 * @brief Change the layout, effective from the next frame boundary (no tearing)
 *
 * Scrolling is a change of `offset` alone.  Applies to whichever framebuffer is
 * active.  Returns ESP_ERR_INVALID_ARG if the layout does not fit.
 */
esp_err_t chipguy_lcd_dpi_panel_set_layout(
    esp_lcd_panel_handle_t panel,
    const chipguy_lcd_dpi_layout_t *layout
);

/**
 * @brief True until the frame boundary that applies the last set_layout()
 */
bool chipguy_lcd_dpi_panel_is_layout_pending(esp_lcd_panel_handle_t panel);

/**
 * @brief Get the virtual framebuffer height
 *
 * @param panel Panel handle
 * @return Virtual height in rows (for wrap-around calculations)
 */
uint32_t chipguy_lcd_dpi_panel_get_virtual_height(esp_lcd_panel_handle_t panel);

/**
 * @brief Get the visible display height
 *
 * @param panel Panel handle
 * @return Display height in rows
 */
uint32_t chipguy_lcd_dpi_panel_get_display_height(esp_lcd_panel_handle_t panel);

/**
 * @brief Get the display width
 *
 * @param panel Panel handle
 * @return Display width in pixels
 */
uint32_t chipguy_lcd_dpi_panel_get_display_width(esp_lcd_panel_handle_t panel);

/**
 * @brief Get total frame count since initialization
 *
 * Useful for debugging and timing measurements.
 *
 * @return Number of frames completed
 */
uint32_t chipguy_lcd_dpi_panel_get_frame_count(void);

/**
 * @brief Get DMA underrun count since initialization
 *
 * Underruns indicate the DMA couldn't keep up with the display timing.
 * This usually results in visual artifacts (blue screen on this display).
 *
 * @return Number of underrun events
 */
uint32_t chipguy_lcd_dpi_panel_get_underrun_count(void);

/**
 * @brief Wait for next vsync (frame boundary)
 *
 * Blocks until the current frame completes and next frame begins.
 * Use this to synchronize scroll updates with display refresh for smooth animation.
 *
 * @param timeout_ms Maximum time to wait in milliseconds (0 = no timeout)
 * @return true if vsync occurred, false if timeout
 */
bool chipguy_lcd_dpi_panel_wait_vsync(uint32_t timeout_ms);

/**
 * @brief Set active framebuffer for display
 *
 * Switches which framebuffer is being displayed, from the next frame boundary.
 * The layout is unchanged.
 * Use this for double-buffering with LVGL or similar graphics libraries.
 *
 * @param panel Panel handle
 * @param fb_index Framebuffer index (0 or 1 for double-buffering)
 * @param wait If true, wait for vsync before returning
 */
void chipguy_lcd_dpi_panel_set_active_fb(
    esp_lcd_panel_handle_t panel,
    uint8_t fb_index,
    bool wait
);

/**
 * @brief Get number of framebuffers allocated
 *
 * @param panel Panel handle
 * @return Number of framebuffers (1-3)
 */
uint8_t chipguy_lcd_dpi_panel_get_num_fbs(esp_lcd_panel_handle_t panel);

/**
 * @brief Get frame timestamps for scanline estimation
 *
 * Returns timestamps (in microseconds since boot) of the two most recent frame
 * completions. One timestamp is updated on even frames, one on odd frames.
 * Use these to estimate current scanline position for tear-free rendering.
 *
 * @param ts_even Pointer to receive even frame timestamp (may be NULL)
 * @param ts_odd Pointer to receive odd frame timestamp (may be NULL)
 */
void chipguy_lcd_dpi_panel_get_frame_timestamps(int64_t *ts_even, int64_t *ts_odd);

/**
 * @brief Estimate current scanline position
 *
 * Uses frame timestamps to estimate which row the display is currently drawing.
 * This allows "race-the-beam" techniques to update scroll registers or framebuffer
 * content after the beam has passed, avoiding the need to wait for vsync.
 *
 * @param panel Panel handle
 * @param confidence_us Optional pointer to receive confidence value (microseconds
 *                      until the estimate becomes stale, i.e. until next frame boundary)
 * @return Estimated row (0 to v_pixels-1), or -1 if estimation not possible
 */
int32_t chipguy_lcd_dpi_panel_estimate_scanline(
    esp_lcd_panel_handle_t panel,
    int32_t *confidence_us
);

#ifdef __cplusplus
}
#endif
