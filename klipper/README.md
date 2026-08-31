# Klipper side

Two files. Both are optional in the sense that the firmware runs without them —
it falls back to compiled-in defaults — but the homing animation is noticeably
better with them.

| File | What it is |
|---|---|
| `KNOMI.cfg` | superset of BTT's stock `KNOMI.cfg`; drop-in replacement |
| `knomi_query_refresh.py` | Klipper extra, lets a macro raise the status rate |

## Install

```bash
# config: include it the way you included BTT's
[include KNOMI.cfg]

# extra: symlink rather than copy, so a Klipper update cannot clobber it
ln -sf /path/to/KNOMI/klipper/knomi_query_refresh.py ~/klipper/klippy/extras/
# then uncomment [knomi_query_refresh] at the bottom of KNOMI.cfg
sudo systemctl restart klipper
```

**`FIRMWARE_RESTART` is not enough after adding or editing the extra.** It
rebuilds the config and printer objects but does not re-import Python modules —
the old code keeps running, silently, with no error. Clearing `__pycache__`
does not help either, because the stale module is already imported. Restart the
service.

## What the extra does, and why it cannot be a macro

Klipper has **two** independent 250ms periods:

* `webhooks.SUBSCRIPTION_REFRESH_TIME` — how often status is **served**. A
  one-shot `objects/query` blocks until the next tick, which is the entirety of
  the ~250ms an HTTP status call costs.
* `motion_report.STATUS_REFRESH_TIME` — how often `live_position` is
  **recomputed**. `get_status` returns a cached value until then.

Changing only the first makes clients fetch the same number more often and
improves nothing. The extra moves both together and restores both on shutdown
and disconnect, so a macro that raises between set and reset cannot leave the
machine running its status loop fast.

Both are module globals read per call rather than captured at startup, so
rebinding takes effect on the next tick — but Klipper's Jinja can neither
import nor rebind globals, so this cannot be a `gcode_macro`.

`_KNOMI_QUERY_RATE` in `KNOMI.cfg` recomputes the rate from the state flags
after every transition. It is written that way rather than as paired set/reset
calls because these operations nest: `QUAD_GANTRY_LEVEL` homes on the way in
when the machine is not homed, and the inner `G28`'s reset would otherwise drop
the rate while the outer level is still running.

## Cost

Measured on a Pi 4 with 495 printer objects, while animating:

| period | display update rate | klippy CPU |
|---|---|---|
| 0.25 (stock) | 3.00/s | 14.1% |
| 0.10 (default here) | 6.75/s | 20.4% |
| 0.05 | 7.75/s | 24.9% |

It applies to **every client on the printer**, not just the KNOMI, and runs
inside `reactor.assert_no_pause()`. Scoped to homing/probing/QGL that is
seconds at a time.

It does **not** endanger multi-MCU homing, which is the tight path on a machine
whose probe lives on a different MCU from its Z steppers (`TRSYNC_TIMEOUT` is
25ms there rather than 250ms). That keepalive is sent from the serialqueue's C
background thread in `chelper/trdispatch.c`, off the Python reactor and out
from under the GIL. Verified: a full `G28` including a multi-MCU Z tap at a
0.05s period completed clean, with `print_stall=0` and no fault lines.

## Extending it

Wrap these macros from your own config rather than editing `KNOMI.cfg`, so it
stays diffable against upstream. One drift worth avoiding, seen in the wild:
dropping `{rawparams}` from the `BED_MESH_CALIBRATE` or `QUAD_GANTRY_LEVEL`
wrappers silently discards every parameter passed to them — which breaks
adaptive meshing (KAMP) and `PROFILE=` in ways that look like the feature
simply not working.
