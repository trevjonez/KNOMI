# What this fork changes

Fork of `bigtreetech/KNOMI` (`firmware` branch), running on a KNOMI2 attached to
a Voron 2.4. Upstream's shipped prebuilt is stale in a way its version string
hides: compiled at `0dbd0bb` (2023-12) and never rebuilt after `791673f`
(2024-07, "Fix black screen"), which pins `platform = espressif32@6.4.0`. Both
report `FW_VERSION V1.0.2`. Building from source is the only way to get that fix.

## Changes

**Busy states draw the real toolhead.** `HOMING`, `PROBING` and `QGLING` no
longer play GIFs; they composite a bed-and-toolhead scene from live position.
Full write-up in [toolhead-scene.md](toolhead-scene.md).

**Screen transitions are capped at 180ms.** Every `_ui_screen_change()` call
site — ~40 of them, nearly all in SquareLine-generated `ui.c` — hardcodes 500ms,
which was the single biggest contributor to how sluggish the device felt.
Clamped centrally in `_ui_screen_change()` rather than by editing generated
files, so it survives regenerating `ui.c`. Sites passing 0 stay instant. See
`UI_SCREEN_ANIM_MS` in `src/config.h`.

**WiFi recovery is bounded instead of one-strike.** Stock flips to AP mode in
RAM after a single failed join and never re-arms a retry, so a brief outage left
the device sitting in its config portal until power-cycled — the mode flip is
RAM-only, which is why a power cycle was the only cure. It now backs off and
retries (10/20/40/60s), falling back to the portal only after
`WIFI_STA_MAX_FAILS` consecutive failures, so a genuinely wrong password is
still recoverable without a USB reflash. Power saving is also disabled.

**Blocking HTTP moved off the UI thread.** Roller list fetches ran on the LVGL
task, so a slow or absent printer froze the screen. `moonraker_task` does the
request now and the UI applies the result.

**Draw buffers moved to internal SRAM.** Was a single full-screen 115KB buffer
in PSRAM via `LV_MEM_CUSTOM_ALLOC`, so every render wrote into PSRAM and every
flush read it all back over the octal bus — LVGL's scattered read-modify-write
blending is its worst case. Now two 40-line buffers in internal DMA-capable
SRAM, with a PSRAM fallback if the allocation fails. `LV_MEM_CUSTOM_ALLOC` stays
on `ps_malloc`: the object tree and GIF decode buffers are fine there, it was
only the hot draw path that was not.

**The Moonraker connection is held open.** Every request used to build its own
`HTTPClient`, so every sample paid a TCP handshake — invisible while Klipper's
250ms status tick dominated, but the limiting cost once that tick is shortened,
and most of the reason the radio was busy. One connection now serves 100+
responses. Note `HTTPClient::end()` closes the socket whatever `setReuse()`
says, so it is called only on failure. Scoped to the status path, which lives
entirely in `moonraker_task`; POSTs run on another task and keep their own
client, since one `HTTPClient` cannot be driven from two tasks.

**Input polled at 10ms** (`LV_INDEV_DEF_READ_PERIOD`), with the CST816S report
rate matched to it.

### Considered and rejected

**`LV_COLOR_16_SWAP=1`.** Looks like a free win — it would let the display push
pixels without byte-swapping. It would also silently corrupt all 28 generated
`LV_IMG_CF_TRUE_COLOR` assets, which are baked in the opposite byte order.
`src/ui/ui.c` has an `#error` guard for exactly this. The per-pixel loop
sometimes cited as the reason to do it is a different overload, for 3-byte RGB
displays; `pushSwapBytePixels` already batches 32px per 512-bit transaction.

**Updating the dependency versions.** Deliberately not done. `platformio.ini`
pins `espressif32@6.4.0`, `lvgl 8.3.7`, `ArduinoJson ^6.19.4` and
`TFT_eSPI 2.5.0`, all years old, and the platform pin in particular is where
IDF/lwIP/mbedTLS security fixes would arrive.

It is left alone because the payoff is speculative and the risk is not. Upstream
pinned `espressif32@6.4.0` on purpose in `791673f` to fix a black screen, so a
platform bump is the most likely thing to break the display outright.
ArduinoJson 7 is a breaking API change touching five call sites in
`moonraker.cpp`. LVGL 9 is a rewrite; 8.4 is minor and would at least supply a
real `lv_gif_pause()` in place of the widget-struct workaround here.

If it is ever revisited, it wants to be its own pass with a flash and a
hardware check after each bump, not a batch version edit.

## Building

Neither PlatformIO nor the ESP32 toolchain needs to be on the host:

```bash
./tools/docker/run.sh          # -> .pio/build/knomiv2/firmware.bin
./tools/docker/run.sh assets   # regenerate the toolhead scene sprites
./tools/docker/run.sh shell    # poke around
```

A baseline build of the unmodified tree lands within 496 bytes of BTT's
prebuilt, with bootloader and partition images byte-identical, so the container
reproduces their build config.

## Flashing

OTA. The upload needs an `MD5` form field alongside the file, and the device
rejects a mismatch before writing anything:

```bash
BIN=.pio/build/knomiv2/firmware.bin
curl -F "MD5=$(md5sum $BIN | cut -d' ' -f1)" -F "firmware=@$BIN" \
     http://knomi.local/update      # -> "OK", then it reboots (~5s)
```

**Confirm the binary before uploading.** The build directory is shared between
targets, so it is easy to upload a stale image and have it look like a silent
failure. Check `stat -c%s` and grep the expected asset bytes out of the `.bin`
rather than trusting the last build to have been the one you meant.

**`/update/identity` cannot tell you which build is running.** It reports
`V1.0.2` either way — that string is `FW_VERSION` in `src/config.h`, one shared
constant across both build targets and both stock and custom builds. Bump it if
that ever needs to be distinguishable remotely.

## Klipper side

Both live in [`klipper/`](../klipper/README.md) in this repo. `KNOMI.cfg` there
is a superset of BTT's stock one and a drop-in replacement for it:

* **`_KNOMI_HOME_INFO`** publishes homing speeds out of the live `printer.cfg`
  so the display need not assume them. Without it the firmware falls back to
  Voron 2.4 defaults.
* **`knomi_query_refresh`**, a small Klipper extra, raises Klipper's status
  refresh rate for the span of an animated state and restores it after. It has
  to move **two** module globals — `webhooks.SUBSCRIPTION_REFRESH_TIME` and
  `motion_report.STATUS_REFRESH_TIME` — because the first governs how often
  status is served and the second how often position is recomputed. Changing
  only one makes clients fetch the same value more often and improves nothing.

Neither is required; stock `KNOMI.cfg` works. Note the extra needs
`systemctl restart klipper`, not `FIRMWARE_RESTART`, which does not re-import
Python modules.
