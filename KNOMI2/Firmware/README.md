# KNOMI2 firmware images

**`knomi2_firmware.bin` in this fork is a custom build, not BTT's.** The stock
image it replaced is kept beside it as `knomi2_firmware_stock.bin`.

| File | MD5 | Size | What it is |
|---|---|---|---|
| `knomi2_firmware.bin` | `6fd07e6a03937f3a23629cd9cd60d337` | 2,792,576 | this fork |
| `knomi2_firmware_stock.bin` | `9d4f51d727c510d6a3cad2985d065852` | 2,921,248 | BTT's original |
| `knomi2_bootloader.bin` | `f2a433bfb512f5aa8dd3e45e3ab091cb` | 15,088 | unchanged from BTT |
| `knomi2_partitions.bin` | `f5ddd8b6ba813771c150ab2df51efa1d` | 3,072 | unchanged from BTT |

Only the application image differs. A build of this fork produces a bootloader
and partition table byte-identical to BTT's, so those two files are theirs
untouched and there is no need to reflash them.

## What the custom build does

`HOMING`, `PROBING` and `QGLING` no longer play canned animations — they draw a
bed-and-toolhead scene composited from the printer's **real toolhead position**,
with the toolhead drawn as a Voron StealthBurner. Also included: much faster
screen transitions, bounded WiFi recovery instead of stock's one-strike flip to
AP mode, and assorted responsiveness fixes.

Details: [`docs/fork-notes.md`](https://github.com/trevjonez/KNOMI/blob/stealthburner-icons/docs/fork-notes.md)
and [`docs/toolhead-scene.md`](https://github.com/trevjonez/KNOMI/blob/stealthburner-icons/docs/toolhead-scene.md)
on the `stealthburner-icons` branch, where the source lives.

## Flashing

**KNOMI2 only.** The V1 has a different pinout and no touch controller.

Over the air, from the device's own `/update` page, or by curl — the upload
needs an `MD5` form field alongside the file, and the device verifies it before
writing anything:

```bash
BIN=knomi2_firmware.bin
curl -F "MD5=$(md5sum $BIN | cut -d' ' -f1)" -F "firmware=@$BIN" \
     http://knomi.local/update      # -> "OK", then it reboots (~5s)
```

To go back to stock, do the same with `knomi2_firmware_stock.bin`.

**`/update/identity` cannot tell you which of these is running.** It reports
`V1.0.2` for all of them, stock included — that string is `FW_VERSION` in
`src/config.h`, one shared constant. The MD5s above are the only way to be sure
what you have.

## Klipper side

Works with the stock `KNOMI.cfg`. The homing animation is more accurate if you
also add the optional `_KNOMI_HOME_INFO` macro, which publishes homing speeds
out of your live `printer.cfg` so the display does not have to assume them —
see `docs/toolhead-scene.md`. Without it, the firmware falls back to Voron 2.4
defaults.
