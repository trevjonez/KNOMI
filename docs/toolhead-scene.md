# The live toolhead scene

`HOMING`, `PROBING` and `QGLING` do not play animations. They share one scene
composited at runtime from the printer's real toolhead position, so during a
probe, a quad gantry level or a home, the head on the screen is where the head
on the machine is.

Source: [`src/ui_overlay/lv_toolhead_scene.cpp`](../src/ui_overlay/lv_toolhead_scene.cpp).
Sprites: [`src/sprites/`](../src/sprites), generated — see
[`tools/stealthburner/`](../tools/stealthburner/README.md).

| Piece | What it is |
|---|---|
| bed | the original `gif_qgling` bed art with the toolhead erased, static |
| shadow | an ellipse on the bed at the X/Y point under the nozzle |
| head | 23 StealthBurner sprites, one per 1px of apparent height |
| label | the state name, under the bed |

All four are children of `ui_ScreenMainGif` and share it with `ui_img_main_gif`;
exactly one of the two is visible at a time.

## Where the data comes from

One Moonraker query, the same one that already drives screen selection, ~462
bytes:

```
/printer/objects/query
    ?gcode_macro%20_KNOMI_STATUS      state flags
    &motion_report=live_position      where the toolhead actually is
    &toolhead=axis_minimum,axis_maximum,homed_axes
    &gcode_macro%20_KNOMI_HOME_INFO   homing speeds, from the live printer.cfg
```

Two things worth knowing about that URL:

- **`motion_report.live_position`, not `toolhead.position`.** The latter is the
  last *commanded* point and runs ahead of real motion while a move is queued.
- **Field selection matters.** A bare `motion_report` also returns the stepper
  and trapq name lists, several hundred bytes nothing here reads.

Klipper answers a multi-object query for the same ~250ms as a single one, so
position costs no extra request. The *cycle* did need work, though: three
sequential queries gave the scene ~1.2Hz, which steps visibly. While the scene
is up, `http_get_loop()` makes only this one query — the ready flag and
temperatures are not on that screen — which reaches ~4Hz. Samples are eased
into, with the easing time tracking the measured interval, so a slower printer
stretches the motion instead of stalling.

## Why a query costs 250ms, and why 4Hz is the ceiling

Worth understanding before trying to make the scene smoother, because the
obvious optimisations do nothing.

`objects/query` takes ~250ms. **Neither the network nor Moonraker is
responsible.** Measured on a Pi 4 at load 0.30:

| | |
|---|---|
| TCP connect | 0.2 ms |
| `/server/info` (Moonraker only, no Klippy) | 5.7 ms |
| Klippy `info` direct on its unix socket | 0.2 ms |
| Klippy `objects/list` direct | 1.3 ms |
| Klippy `objects/query` direct | **250.8 ms** |

Bypassing Moonraker entirely and talking to `klippy.sock` gives the identical
quantum, so it is Klipper, and it is one specific endpoint.

The cause is `SUBSCRIPTION_REFRESH_TIME = .25` in `klippy/webhooks.py`. A
one-shot query appends itself to `pending_queries` and then blocks:

```python
self.pending_queries.append((None, objects, complete.complete, {}))
if self.query_timer is None:                    # only when absent
    qt = reactor.register_timer(self._do_query, reactor.NOW)
msg = complete.wait()                           # waits for the tick
```

It only gets an immediate `NOW` timer **if no timer already exists**. Moonraker
holds a permanent subscription, so one always does, and the query waits for the
next 250ms tick. Confirmed by prediction: sleep N ms before querying and
latency is `250 − N`, measured within 1ms at N = 0/50/100/150/200.

This is deliberate, and it is protection rather than throttling. In
`_do_query`, all subscribers and all pending queries are merged into one pass;
results go into a shared per-tick `query` dict consulted before any object is
touched, so ten clients asking for `toolhead` cost exactly **one**
`get_status()`. The whole pass runs inside `reactor.assert_no_pause()`, so
status collection cannot yield into motion-critical work. Subscribers receive
only changed fields. A query storm therefore cannot make Klipper do more work —
extra clients ride the tick that was already scheduled.

**So 4Hz is not a limit imposed on us, it is the granularity of Klipper's
status for everyone.** Polling harder just queues more waiters onto the same
tick. The scene can be up to 250ms stale, and no faster host, network or
Moonraker changes that.

### If it ever needs to be faster

Two levers, and they compose in one order only.

**Subscribe instead of poll.** Measured against `printer.objects.subscribe`
while the toolhead was moving: pushes arrive every **251ms median** (the same
tick) at **175 bytes** mean, versus 462 bytes and a blocking request per sample
for the polled form. Same data rate, but nothing waits and nothing reconnects.

Two behavioural differences to design around: updates are **deltas**, so the
client must hold state and merge rather than replace; and an idle printer sends
**nothing at all** — subscribing and watching a stationary machine for six
seconds produced zero pushes. There is no heartbeat, so liveness needs the
websocket's own ping/pong. Needs a websocket client, which is not vendored.

**Then, optionally, shorten the tick.** `SUBSCRIPTION_REFRESH_TIME` is a module
global read on every `_do_query`, so it can be changed at runtime — not from a
`gcode_macro` (Jinja cannot import or rebind globals) but from a small Klipper
`extras` plugin, which can `import webhooks` the same way `motion_report.py`
does `import chelper`:

```python
# klippy/extras/query_refresh.py
import webhooks
class QueryRefresh:
    def __init__(self, config):
        config.get_printer().lookup_object('gcode').register_command(
            'SET_QUERY_REFRESH', self.cmd, desc="Set status refresh period")
    def cmd(self, gcmd):
        webhooks.SUBSCRIPTION_REFRESH_TIME = gcmd.get_float(
            'PERIOD', minval=0.02, maxval=2.)
def load_config(config): return QueryRefresh(config)
```

The cost is smaller than it looks: a status pass over **all 495** objects on
this machine measures **~3.7ms**, and most of that is encoding the 437KB
response that only a full poll receives — a subscriber's delta pass is far
cheaper. At a 100ms tick that is a low single-digit percentage of the reactor.

But it applies to **every client on the printer**, and it runs inside
`reactor.assert_no_pause()`, so it is non-yielding work made more frequent
while the host is also feeding the step queue. And on its own it does not help
a polling client, which would then need to poll at 10Hz with a TCP connect
each time.

So: subscription first, because it makes a shorter tick free for this device
rather than merely faster.

## The projection

The bed's top face is a trapezoid, and that trapezoid *is* the mapping: printer
X/Y interpolates onto it, and the head foreshortens by exactly the ratio the bed
does. The corners are measured off the artwork by `gen_sprites.py` and written
into `src/sprites/toolhead_scene.h`, so the C follows automatically if the bed is
ever redrawn. Nothing is written down twice.

A row's centre and half-width are both linear in depth. A true projective
transform would put a slight curve in it, but over 13 pixels of depth that is
under half a pixel.

Two deliberate departures from truth, both at their constants in the source:

- **Z is exaggerated.** To scale the bed is ~0.45 px/mm, so a 10mm probe hop
  would be 4px — honest and invisible. It renders at 1.35 px/mm with a soft
  asymptotic ceiling, so 10mm reads as ~12px and a park at Z=250 compresses
  instead of flying off screen.
- **The shadow exists to make Z readable.** In a 3/4 view, rising off the bed
  and moving toward the viewer travel the same direction on screen. They are
  only distinguishable because something stays behind on the bed.

## Homing: Klipper lies, but predictably

Homing looks like the case where position is unknowable. It isn't — but the
obvious approaches are wrong, and both were shipped and caught on hardware
before measuring settled it.

To home an axis, Klipper **force-sets the position** to
`position_endstop -/+ 1.5 x travel`, well outside the axis range, then ramps
toward the endstop. Sampled on a Voron 2.4 during a real `G28 X`, head parked
at X20:

| t (s) | `live_position[0]` | `homed_axes` | `homing` |
|---|---|---|---|
| 0.00 | 20.00 | `xyz` | false |
| 1.47 | **−183.00** | `xyz` | true |
| 6.22 | 1.53 | `xyz` | true |
| 9.99 | 151.91 | `xyz` | true |
| 10.24 | **351.00** | `xyz` | true |
| 10.49 | 341.00 | `xyz` | true |

−183 is `351 − 1.5 × 356`, exact.

**That reading is not noise.** It is offset from the truth by a *constant*,
because the ramp and the toolhead travel together. So `live − origin` is the
real distance covered, and adding it to where the axis was last seen recovers
where it is now:

```
physical = last_known + (live - origin)
```

No timer, no endstop positions, no assumed direction — the animation is driven
by measured motion. `origin` is captured from the first reading that falls
outside the axis limits, which only a force-set coordinate ever does.

### Two traps

**`homed_axes` cannot be used here.** It flips to "homed" when the force-set
happens — the *start* of the move — and on a machine that was already homed it
never changes at all. In the capture above it reads `xyz` unchanged through the
entire `G28 X`. Keying anything on it pinned the head at the bed edge for 4.8s
while the ramp climbed back into range.

**`last_known` must freeze during homing.** It is recorded in the Moonraker
layer whenever `homed_axes` vouches for an axis — which, per above, is always.
So it absorbed the −183 at the same sample the ramp was latched, and
`start + (live − origin)` collapsed algebraically to plain `live`: the
reconstruction ran and did nothing. This fix is invisible in a diff of the
reconstruction itself; it lives in `moonraker.cpp`.

Replaying the captured trace through both versions:

| variant | samples pinned at bed edge | longest dwell |
|---|---|---|
| trust `live` | 19/39 | 4.8s |
| reconstruct, `last_known` unguarded | 19/39 | 4.8s |
| reconstruct + freeze `last_known` | 0/39 | 0.0s |

### What the printer still has to tell us

Only a plausible homing speed, used to recognise the discontinuity when the
endstop trips (a ~199mm jump in one sample) and the reading becomes real. That
handoff matters because `G28` moves to bed centre before homing Z — without
spotting it, the head would stay stuck in the corner through that move.

That comes from `_KNOMI_HOME_INFO`, a macro that publishes values out of the
live `printer.cfg` so retuning the printer needs no reflash:

```ini
[gcode_macro _KNOMI_HOME_INFO]
variable_ready: False
variable_x_home: 0.0
variable_y_home: 0.0
variable_x_speed: 0.0
variable_y_speed: 0.0
variable_z_speed: 0.0
gcode:

[delayed_gcode _KNOMI_HOME_INFO_INIT]
initial_duration: 2
gcode:
    {% set cf = printer.configfile.settings %}
    ...SET_GCODE_VARIABLE from cf['stepper_x'] etc...
```

Every read must be guarded with `.get()`: `position_endstop` is **absent** on a
probe-homed axis (Z on any Voron with a probe as its Z endstop), and
`homing_speed` has a Klipper default.

**The macro is optional.** Without it `ready` reads false and the firmware uses
compiled-in Voron 2.4 defaults, so an unmodified printer still animates.

## Tuning

All in `lv_toolhead_scene.cpp`:

| Constant | Does |
|---|---|
| `SCENE_BED_Y` | where the bed sits vertically on the 240×240 screen |
| `SCENE_Z_NEAR_PX_PER_MM`, `SCENE_Z_MAX_PX` | Z exaggeration and its soft ceiling |
| `SCENE_LERP_MIN_MS`, `SCENE_LERP_MAX_MS` | easing bounds between samples |
| `SCENE_LABEL_COLOR` | `#C02F30`, sampled from the bed art |
| `SCENE_LABEL_EMBOSS_PX` | fake-bold offset; `0` disables it |
| `SCENE_HOME_SNAP_FACTOR`, `SCENE_HOME_SNAP_MIN_MM` | endstop-trip detection |
| `SCENE_HOME_FALLBACK_*_SPEED` | used only until `_KNOMI_HOME_INFO` answers |

## Gotchas

- **A hidden `lv_gif` still decodes.** Hiding the object stops it being drawn,
  not decoded — its frame timer keeps calling the GIF decoder. LVGL 8.3 has no
  `lv_gif_pause()` (8.4 added it), so the timer is stopped through the widget
  struct, which `lv_gif.h` makes public. Only ever undo your own pause:
  `lv_gif` pauses that timer itself when a finite-loop animation ends.
- **Enabling a font in `lv_conf.h` does not rebuild LVGL.** The header lives
  outside the library directory, so PlatformIO keeps its cached objects and the
  link fails with `undefined reference to lv_font_montserrat_NN`. Clear
  `.pio/build/<env>/lib*/lvgl`.
- **LVGL has no bold Montserrat** and no synthetic bold. The label is drawn
  twice, one pixel apart.
- **Logs will not help you debug this.** Klipper logs no toolhead position
  during homing, and Moonraker's log holds only HTTP requests and connection
  events. `klippy.log` is good for the config Klipper actually parsed
  (`homing_positive_dir`, `homing_speed`, `position_endstop`) and nothing else.
  Every behavioural fact here came from sampling the live API through a real
  `G28` and replaying the capture offline.

## Verifying without a flash

[`tools/stealthburner/preview_scene.py`](../tools/stealthburner/preview_scene.py)
mirrors `scene_apply()` and renders the bed corners and a Z sweep to a contact
sheet, reporting how close each lands to the round display's 120px radius. It
caught the shadow spilling off the bed edge onto the background, where it read
as a bite taken out of the bed.

Know its limits. It validates the projection maths, not the LVGL object
lifecycle — "is the scene shown at the right time" is a question only hardware
answers, and that is exactly where one shipped bug lived (the scene drew over
the idle screen at boot because the init call to hide it early-returned on a
"no change" guard).

And a replay is only worth what it models: an earlier version of the homing
replay hardcoded the start position instead of modelling `last_known`, so it
reported a fix that had in fact changed nothing.
