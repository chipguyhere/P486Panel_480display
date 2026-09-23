# chipguy_P486Panel_480display

A self-contained **display** driver for the **Waveshare ESP32-P4-86-Panel-ETH-2RO** —
the 86mm wall-panel board with a **720×720 MIPI-DSI LCD** and a **capacitive touch**
panel, on an ESP32-P4 with **32 MB flash** and **32 MB PSRAM**. It drives the panel so
you don't have to think about it: the hardware is fixed and fully handled, leaving you
to write an **LVGL 9** app and nothing else.

LVGL renders at a logical **480×480**, and the ESP32-P4's **PPA** scales every frame up to
the panel's native 720×720 in hardware. That's the point of this library: 480×480 is a
common size across small display boards, so a UI written here drops onto other 480×480
panels unchanged, and needs a quarter fewer pixels to render. If you'd rather use the full
native resolution, use the sibling library
[`chipguy_P486Panel_display`](../chipguy_P486Panel_display).

The bundled examples are the heart of the library. The two **starting points** are
complete and working — the display, touch, and LVGL are already wired up and running
before your code gets control. You pick the one that matches how you want to build your
UI, copy it, and start replacing its placeholder UI with your own. Two further examples,
**LvglCalculator** and **LvglBouncingBalls**, are finished apps you can read for ideas.

---

## Installation

This library is distributed by downloading the code as a ZIP from GitHub —
there is no Arduino Library Manager entry. To install:

1. On the GitHub project page, click **Code ▸ Download ZIP**.
2. Unzip it, and move the resulting folder into your Arduino **libraries**
   directory:
   - macOS / Linux: `~/Documents/Arduino/libraries/`
   - Windows: `Documents\Arduino\libraries\`
3. Restart Arduino IDE so the examples will appear in the File menu.

(Alternatively, in the IDE: **Sketch ▸ Include Library ▸ Add .ZIP Library…**
and select the downloaded ZIP.)

You also need the companion **`chipguy_P486Panel_touch`** library, installed the same
way — the examples bring up touch through it.

### Dependency: LVGL 9

The examples need **LVGL 9**. Install it via the Arduino **Library Manager**
(search "lvgl"). Each example sketch folder ships its own `lv_conf.h`; LVGL reads
its configuration from that file, so keep it next to the `.ino`.

This library was developed against LVGL 9.3 (the current version of LVGL that is
compatible with SquareLine) but is very likely compatible with later versions.

### Arduino IDE board settings

This board uses a generic ESP32-P4 board definition. In **Tools**, set:

- **Board:** ESP32P4 Dev Module
- **PSRAM:** Enabled  *(required — the framebuffers live in PSRAM)*
  There is no size setting; "Enabled" is the whole choice, and the board's 32 MB is
  what you get.
- **Flash Size:** 32 MB (256 Mbit)  *(required — the bundled `partitions.csv` assumes it)*
The Flash Size check lives in `lv_setup_P486Panel_480.hpp` rather than in the display
header, so that a sketch can waive it with `#define CHIPGUY_ALLOW_ANY_FLASH_SIZE`
before the include.  That is meant for the uncommon sketch that builds one source
tree for this board *and* a board with a smaller flash, and so has to name a single
Flash Size for both; waiving it makes the partition table your responsibility.  A
sketch that only targets this board should leave it alone.
- **USB CDC On Boot:** depends on which USB-C port you plug into — see below.

#### Which USB-C port, and what it costs

The board has two USB-C ports, and they want opposite settings:

| Port | What it is | USB CDC On Boot | Internal DRAM |
| --- | --- | --- | --- |
| **Bottom** | CH340 USB-serial bridge into UART0 | **Disabled** | ~50 KB cheaper |
| **Top** | the ESP32-P4's native USB | **Enabled** | ~50 KB dearer |

Enabling CDC builds the USB stack into the image, which costs roughly 50 KB of
**internal DRAM** — the scarce resource on this board (PSRAM is plentiful; internal
DRAM is what runs out first, and it is what DMA descriptors, WiFi and TLS buffers
compete for). If you are using the bottom port, disabling CDC is free margin.

> **Serial note:** these examples print with **`Serial0`**, which is **always UART0**
> — that is, always the **bottom** port — no matter how "USB CDC On Boot" is set.
> The core guarantees this: *"There is always Serial0 for UART0"*.
>
> What the setting actually changes is plain **`Serial`**:
>
> - **CDC Enabled** → `Serial` is the native-USB CDC device (**top** port)
> - **CDC Disabled** → `Serial` is an alias for `Serial0` (**bottom** port)
>
> So with the bottom port you can use either name and CDC can stay Disabled. Enable
> CDC only if you want `Serial` output on the top port, and expect to pay the DRAM.

---

## Rotation

Mounting the panel on its side is one line, before `lv_setup.begin()`:

```cpp
display.setRotation(90);    // 0, 90, 180 or 270 (counter-clockwise)
lv_setup.begin();
```

Touch is inverse-transformed automatically to match, so your UI coordinates stay in the
upright 480×480 space. Every example ships with this line present but commented out.

Rotation is **free** here. The PPA is already engaged on every flush to scale 480×480 up
to 720×720, and rotation is just another field in that same operation — no extra memory,
no extra pass. (In the native-resolution sibling library there's nothing to scale, so
rotation there costs an extra ~2 MB of buffers and a PPA pass per frame.)

---

## The examples

Open them from **File ▸ Examples ▸ chipguy_P486Panel_480display**. All come up running on
the hardware immediately, so you can confirm your board works before you change a line.

To turn one into your own project, **save it under a new name** (File ▸ Save As) so your
work lives outside the library, then start editing. Each example folder is
self-contained — it carries its own `lv_setup.hpp`, `lv_conf.h` and `partitions.csv` —
so a copied sketch is fully standalone.

### LvglClaudeCodeStub — write the UI yourself (by hand or with an AI)

Use this when you want to build the interface **in code**: directly, or by
handing the sketch to an AI coding assistant such as Claude Code.

The UI lives in a tiny `ui.h` / `ui.cpp` pair in the sketch folder. All the
hardware/LVGL setup happens in the sketch before `ui_init()` is called, so
`ui_init()` is a blank canvas: it receives a live LVGL screen and you create
widgets on it. To start your app:

1. Open `ui.cpp` and look at `ui_init()`. The demo it ships with is a **bring-up test** —
   a "Hello, world!" label that follows your finger, and a rectangle cycling
   RED ▸ GREEN ▸ BLUE once a second. Between them they prove touch alignment and panel
   color order (and the finger-follow is the quickest way to check a rotated build).
   Once you trust the hardware, **delete it.**
2. In its place, build your own UI on `lv_screen_active()` using normal LVGL
   calls (`lv_label_create`, `lv_button_create`, event callbacks, …).
3. As your app grows, add more screens and helper functions, and declare the
   ones the sketch needs in `ui.h`.

Because everything except the UI is already wired up, this scaffold is ideal to
hand to **Claude Code**: point it at the sketch folder and describe the app you
want — it can focus entirely on `ui.cpp` and the screens you add, without
touching display/touch/LVGL plumbing.

**Reach for this one** when you want full control, a tiny footprint, or
AI-assisted/hand-written UI code.

### LvglCalculator — a worked, resolution-independent example

A working four-function calculator styled after the dark iOS calculator. Its entire
layout — key diameter, gaps, margins, and font sizes — is derived from the live display
resolution at runtime, so nothing is pinned to 480×480. It's a good read for how to build
a UI in plain LVGL that travels between panels of different sizes. Not a blank starting
point — copy it if you want a calculator, or just study `build_keypad()` in `ui.cpp`.

### LvglBouncingBalls — physics and live controls

1–20 circular balls bounce under gravity, each a random color, re-launching to a random
height on every floor bounce. Two sliders at the bottom set the ball count and the speed.
Also fully resolution-independent, and a compact example of driving continuous animation
from `lv_timer_handler()` while touch input stays responsive.

### Lvgl93SquarelineLauncher — design the UI visually in SquareLine Studio

Use this when you'd rather **design the interface visually** in
[SquareLine Studio](https://squareline.io/) (a drag-and-drop LVGL UI editor) and
just run the result on the board.

The sketch's `src/` folder holds a small **placeholder** UI in the exact file
shape a SquareLine export produces (`ui.c`, `ui_Screen1.c`, …). The sketch calls
the `ui_init()` that SquareLine generates. To start your app:

1. In SquareLine Studio, create a project sized **480×480**, **16-bit** color,
   with **no rotation or offset**.
2. Lay out your screens visually.
3. **Export ▸ UI files** (the "Arduino TFT_eSPI profile" works, among others).
4. **Replace the whole `src/` folder** with your export. Build and upload —
   `ui_init()` now runs your design.

Treat `src/` as disposable: you overwrite it wholesale each time you re-export
from SquareLine, so don't keep anything there. Your own application logic lives
in the **sketch** — after calling `ui_init()`, attach event handlers from the
`.ino` to the UI objects SquareLine exposes (e.g. `ui_Button1`). That way
re-exporting the visuals never touches your code.

**Reach for this one** when you want to iterate on layout visually and write
little or no C for the UI itself.

---

## What the examples give you: the `lv_setup` API

Every example wires the hardware to LVGL through one small header, **`lv_setup.hpp`**
(plus **`lv_conf.h`** for LVGL's build config). That header, and the global objects it
defines, is the entire surface you interact with — and once `begin()` returns, you are
just using plain LVGL.

```cpp
#include "lv_conf.h"
#include "lvgl.h"
#include "lv_setup.hpp"

void setup() {
    Serial0.begin(115200);

    // display.setRotation(90);  // optional
    lv_setup.begin();            // initialize display + touch + LVGL

    // ...build your UI on lv_screen_active() here (or call ui_init())...
}

void loop() {
    lv_timer_handler();          // let LVGL run
    delay(5);
}
```

What the header provides:

- **`lv_setup.begin()`** — call once in `setup()`. Brings up the DSI display, the
  capacitive touch, and LVGL; allocates the 480×480 draw buffers and wires them to the
  720×720 DMA framebuffers through the PPA, creates the LVGL display in **direct render
  mode** plus a pointer (touch) input device, and connects them. After this returns, the
  active LVGL screen exists and you build your UI with ordinary LVGL calls.
- **`display`** — a global `chipguy_P486Panel_480display` object. Handy methods:
  `display.width()`, `display.height()` (the logical 480×480), `display.nativeWidth()` /
  `display.nativeHeight()` (the panel's 720×720), `display.setRotation(deg)` /
  `display.getRotation()`, and `display.setBacklight(0..100)`.

You don't render, scale, or flush anything yourself — LVGL renders into the draw buffers,
the PPA scales each finished frame into a framebuffer, and the driver flips at vsync
(tear-free, double buffered). Your job is to create LVGL widgets and call
`lv_timer_handler()` in `loop()`.

The example's `lv_setup.hpp` is a thin shim: a long comment plus a single `#include` of
the real implementation in this library's `src/`. Leave it alone and your sketch picks up
library improvements automatically; copy the library header's contents over that
`#include` when you need to take control — for multi-touch (LVGL consumes only one
contact point), or to share the framebuffer with another renderer. The shim's comments
walk through both.

### Portable across chipguy board libraries

Because every hardware detail lives in just two files — **`lv_setup.hpp`** and
**`lv_conf.h`** — your application code (`ui.cpp`, or the SquareLine `src/`
files) is hardware-independent. It only ever talks to LVGL.

So to move an app to a **different display board**, drop in the `lv_setup.hpp`
and `lv_conf.h` from that board's chipguy library, and the rest of your sketch
is unchanged. This library's 480×480 logical size is deliberately a common one, so UIs
written here travel especially well to other 480×480 boards such as
`chipguy_4848S040_display` or `chipguy_ESP32-S3-Touch-LCD-2.1_display`.

---

## Companion libraries

The board's other hardware is handled by separate libraries, so you only pull in what you
use:

- **`chipguy_P486Panel_touch`** — the capacitive touch controller. Required by the
  examples; `lv_setup.begin()` brings it up for you.
- **`chipguy_P486Panel_relays`** — the onboard relays: `begin()`,
  `setRelay(n, state)`, `setRelayEnergized(n)`, `setRelayOff(n)`.

## TF / microSD card slot

The board has a microSD (TF) slot on the ESP32-P4's dedicated 4-bit SD interface. This
library doesn't drive it and doesn't include a filesystem — but **it shares nothing
with the display** (the panel is MIPI-DSI on its own lanes), so `SD_MMC` works alongside
this library in either order. No card-detect switch.

Pinout, the P4-specific notes on card power and speed, and copy-paste `SD_MMC` /
ESP-IDF starting points are in **[`README-sdcard.md` in `chipguy_P486Panel_display`](../chipguy_P486Panel_display/README-sdcard.md)** — same board, same slot, one copy of the details.

---

## License

This library is **mixed-license**:

- The original code (display driver, examples) is **MIT** — see [LICENSE](LICENSE).
- **`esp_lcd_panel_dpi_bb.c` / `.h`** is a fork of ESP-IDF's `esp_lcd_panel_dpi.c`
  (Copyright © Espressif Systems) with hardware-scrolling additions, and is licensed
  under the **Apache License, Version 2.0** — see
  [LICENSE-Apache-2.0.txt](LICENSE-Apache-2.0.txt). Those files carry SPDX headers
  recording both the upstream attribution and the modifications.

The panel initialization/gamma values are standard register settings.
`lv_conf.h` is based on LVGL's template (LVGL is MIT-licensed).
