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

### There are TWO 250ms periods, and they must move together

This is the part that cost a debugging session. `SUBSCRIPTION_REFRESH_TIME`
governs how often status is **served**. `STATUS_REFRESH_TIME` in
`extras/motion_report.py`, also 0.250, governs how often `live_position` is
**recomputed** — `get_status` returns a cached value until then:

```python
def get_status(self, eventtime):
    if eventtime < self.next_status_time or not self.dtrapqs:
        return self.last_status                       # cached
    self.next_status_time = eventtime + STATUS_REFRESH_TIME
```

Shortening only the first makes clients fetch the same number more often and
improves nothing. Measured with just that changed: samples every **0.101s**
while the value still changed only every **0.302s**, in 12.1mm steps. It also
actively broke the homing detector, which was measuring rate per sample — see
Traps below.

**So 4Hz is the granularity of Klipper's status for everyone, not a limit
imposed on this device.** No faster host, network or Moonraker changes it.

### Making it faster, as actually done

`klipper-configs/klippy_extras/knomi_query_refresh.py` rebinds **both** module
globals at runtime, driven from `_KNOMI_QUERY_RATE` so the rate is raised only
for the span of an animated state and restored after. Both are read per call
rather than captured at startup, so rebinding takes effect on the next tick.
This cannot be a `gcode_macro`: Klipper's Jinja can neither import nor rebind
globals.

0.10s is the measured knee, not a guess:

| tick | KNOMI request rate | klippy CPU |
|---|---|---|
| 0.25 (stock) | 3.00/s | 14.1% |
| 0.15 | 5.42/s | 18.3% |
| **0.10** | **6.75/s** | **20.4%** |
| 0.05 | 7.75/s | 24.9% |

0.25 → 0.10 more than doubles the rate for +45% CPU; 0.10 → 0.05 buys 15% more
for another 22%. Below ~0.10 Klipper generates ticks the device cannot consume.

A status pass over all **495** objects here costs only **~3.7ms**, and most of
that is encoding the 437KB response only a full poll receives. But it applies
to **every client on the printer** and runs inside
`reactor.assert_no_pause()`, so it is non-yielding work made more frequent
while the host feeds the step queue. Scoped to homing/probing/QGL it is
seconds at a time, and the extra restores both periods on shutdown and
disconnect so a macro that raises cannot leave the machine running fast.

The device side also holds its Moonraker connection open (`setReuse`, and
crucially *not* calling `end()` on success, which closes the socket regardless).
One connection served 100 responses where each sample previously paid a TCP
handshake — ~30x fewer, and most of the reason the radio was busy.

### Still on the table: subscribe instead of poll

Measured against `printer.objects.subscribe` while the toolhead was moving:
pushes every **251ms median** at **175 bytes**, versus 462 bytes and a blocking
request per sample. Same data rate, but nothing waits.

Two behaviours to design around: updates are **deltas**, so the client must
hold state and merge rather than replace; and an idle printer sends **nothing
at all** — six seconds watching a stationary machine produced zero pushes, so
liveness needs the websocket's own ping/pong. Needs a websocket client, which
is not vendored.

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

### Traps

Four, all of which shipped before being caught on hardware. Every one was
found by capturing a real `G28` and replaying it offline, never by reasoning
about what Klipper ought to report.

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

**The snap detector must measure rate per *change*, not per sample.** Klipper
recomputes `live_position` on its own 250ms cycle — `STATUS_REFRESH_TIME` in
`extras/motion_report.py` — and `get_status` returns a cached value until then.
Poll faster than that and you get runs of identical values followed by one
full-size step. Measured: samples every 0.101s, value changing every 0.302s in
12.1mm steps. A per-sample threshold of `40 × 0.101 × 3` = 12.8mm against a
real 12.1mm step is a 6% margin, and jitter tips it over — the ramp unlatches
mid-sweep, relatches at the current reading, and the head jumps back to its
start. Measuring per change reads 40mm/s at any poll rate.

**The range test needs a margin, because `position_endstop` equals
`position_max`.** X homes to 351 with a maximum of 351. The value Klipper
snaps to on a trigger therefore lands a float-hair *above* the maximum and
reads as a fresh force-set: the axis unlatches on the jump and relatches on the
same sample with the endstop as its origin, rendering `20 + (341 − 351) = 10`
and stranding the head at the left of the bed for the rest of the sequence.
2mm of clearance separates it (the real force-set is ~500mm out), and the
latch is skipped entirely on the sample that just snapped.

Replaying the captured trace through the broken and fixed versions:

| variant | samples pinned at bed edge | longest dwell |
|---|---|---|
| trust `live` | 19/39 | 4.8s |
| reconstruct, `last_known` unguarded | 19/39 | 4.8s |
| reconstruct + freeze `last_known` | 0/39 | 0.0s |

and for the latch guards, what X shows while Y is homing:

| variant | X during Y-home | X range over the G28 |
|---|---|---|
| no margin, no snap guard | never unlatches | −1 … 351 |
| margin + guard | **341 (its real position)** | 20 … 351 |

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
- **`FIRMWARE_RESTART` does not re-import Python modules.** It rebuilds the
  config and the printer objects, but the module stays in `sys.modules`, so an
  edited `extras/*.py` keeps running the old code — silently, with no error and
  no warning. `sudo systemctl restart klipper` is required. Clearing
  `__pycache__` does not help, because the stale module is already imported.
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
