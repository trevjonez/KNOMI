/* Live toolhead scene -- replaces the gif_probing / gif_qgling animations.
 *
 * Those were canned loops: a head bouncing over a bed on a fixed path that
 * had nothing to do with what the printer was doing. This composites the
 * same artwork from the real position instead, so during a probe or a quad
 * gantry level the head on the screen is where the head on the machine is.
 *
 * Three pieces, all children of ui_ScreenMainGif:
 *
 *   bed     the original gif_qgling bed art, static
 *   shadow  an ellipse on the bed marking the X/Y point under the nozzle
 *   head    the StealthBurner, one sprite per depth step
 *
 * The shadow is what makes Z readable: in a 3/4 view a head that rises off
 * the bed and a head that moves towards the viewer travel the same direction
 * on screen, and the two are only distinguishable if something stays behind
 * on the bed. The gap between head and shadow is the Z height.
 *
 * Position arrives at roughly 4Hz (see http_get_loop) so every sample is
 * eased into rather than snapped to, and the easing time tracks the measured
 * sample interval -- if the printer answers slower, the motion stretches to
 * match instead of stepping and stalling.
 */
#include "ui/ui.h"
#include "knomi.h"
#include "moonraker.h"
#include "lv_overlay.h"
#include "sprites/toolhead_scene.h"
#include <math.h>
#include <string.h>

/* Where the bed sits on the 240x240 screen. Low enough to leave room for the
 * head and its Z lift above the back edge, high enough that the bed's front
 * corners stay inside the round display's bezel. */
#define SCENE_BED_X   ((LV_HOR_RES - SB_BED_W) / 2)
#define SCENE_BED_Y   138

// gap between the bottom of the bed slab and the state label
#define SCENE_LABEL_GAP 8

/* The bed art's own top-face red, sampled from the sprite (#C02F30, 1492px,
 * its most common colour; the slab below it is the darker #AC1C1D).
 *
 * Written as a literal rather than lv_theme_color() on purpose: the bed is
 * fixed artwork, so if the user retheme the UI the label must stay matched to
 * the bed rather than drifting away from it. It happens to equal
 * LV_32BIT_BTT_RED, which is where BTT took the theme default from. */
#define SCENE_LABEL_COLOR 0xC02F30

/* LVGL 8.3 ships Montserrat in regular only -- there is no bold face to
 * select and no synthetic bold. Drawing the text twice, offset one pixel,
 * thickens the stems and is the usual way to get a bold-ish look without
 * adding a converted font to the build. */
#define SCENE_LABEL_EMBOSS_PX 1

/* Z rendered with a soft ceiling: linear near the bed at SCENE_Z_NEAR_PX_PER_MM,
 * bending over to approach SCENE_Z_MAX_PX asymptotically so a park at Z=250
 * compresses instead of flying off the top of the screen.
 *
 * The near-bed rate is deliberately larger than true scale. To scale, the bed
 * is ~0.45 px/mm, so a 10mm probe hop would be 4px -- technically honest and
 * practically invisible. 1.35 px/mm makes the same hop 12px and legible; it
 * is the one place this scene exaggerates rather than mirrors, and only the
 * axis that a 3/4 projection renders worst. */
#define SCENE_Z_NEAR_PX_PER_MM  1.35f
#define SCENE_Z_MAX_PX          45.0f

/* Easing bounds, ms. Floor stops a burst of fast samples from looking jittery,
 * ceiling stops a stalled printer from leaving the head drifting for seconds. */
#define SCENE_LERP_MIN_MS  80
#define SCENE_LERP_MAX_MS  700

/* Homing.
 *
 * Not a guess at all -- see homing_target(). Klipper reports a fictitious
 * position while homing, but one that is offset from the truth by a constant,
 * so the distance it covers is real and can be added to where the toolhead
 * was last seen (moonraker.data.last_known) to recover where it is now.
 *
 * The only thing needed from the printer is a plausible homing speed, used to
 * recognise the discontinuity when the endstop trips. _KNOMI_HOME_INFO
 * publishes that from the live printer.cfg, so retuning the machine needs no
 * reflash; the constants below cover a printer without that macro. */

/* Used only until _KNOMI_HOME_INFO answers. Voron 2.4 values. */
#define SCENE_HOME_FALLBACK_XY_SPEED  40.0f   // mm/s
#define SCENE_HOME_FALLBACK_Z_SPEED    8.0f   // mm/s

/* Where the head floats while Z is still unknown, mm. */
#define SCENE_HOME_Z_MM         15.0f

/* Apparent speed above homing_speed x this means the endstop triggered and
 * Klipper replaced the ramp with the real position.
 *
 * Measured against the time since the value last CHANGED, never since the last
 * sample. Klipper recomputes live_position on its own ~250ms cycle
 * (STATUS_REFRESH_TIME in extras/motion_report.py) and serves a cached value in
 * between, so polling faster than that returns runs of identical values
 * followed by one full-size step. Measured per sample, an ordinary 40mm/s
 * homing move looks like 120mm/s when sampled at 3x the rate the value
 * updates -- which tripped this detector mid-sweep, dropped the ramp, relatched
 * at the current position and threw the head back to its start. Measured
 * per change it reads 40mm/s at any sample rate. */
#define SCENE_HOME_SNAP_FACTOR   3.0f

/* How far outside its own limits a reading must be to count as Klipper's
 * force-set coordinate, mm. That value sits ~1.5x the axis travel outside, so
 * any small margin separates it -- but the margin is load-bearing, see the
 * latch test in homing_target(). */
#define SCENE_HOME_FORCEPOS_MARGIN_MM  2.0f

typedef struct {
    float u;    // 0..1 across the bed, left to right
    float v;    // 0..1 front to back
    float z;    // mm above the bed
} scene_pos_t;

static lv_obj_t * scene_bed = NULL;
static lv_obj_t * scene_shadow = NULL;
static lv_obj_t * scene_head = NULL;
static lv_obj_t * scene_label = NULL;
static lv_obj_t * scene_label_emboss = NULL;
static lv_anim_t scene_anim;
static toolhead_scene_mode_t scene_mode = TOOLHEAD_SCENE_OFF;

static scene_pos_t pos_from = {0.5f, 0.5f, 0.0f};
static scene_pos_t pos_to   = {0.5f, 0.5f, 0.0f};
static scene_pos_t pos_now  = {0.5f, 0.5f, 0.0f};

static bool scene_active = false;
static uint32_t last_sample_ms = 0;
static uint32_t last_seq = 0;

static void scene_visibility(bool active);
static void home_reset(void);

/* Set both copies of the state label and re-centre them. Width changes with
 * the text, so alignment has to be redone every time it changes. */
static void scene_label_set(const char * txt) {
    const int y = SCENE_BED_Y + SB_BED_H + SCENE_LABEL_GAP;
    lv_label_set_text(scene_label_emboss, txt);
    lv_label_set_text(scene_label, txt);
    lv_obj_align(scene_label_emboss, LV_ALIGN_TOP_MID, SCENE_LABEL_EMBOSS_PX, y);
    lv_obj_align(scene_label, LV_ALIGN_TOP_MID, 0, y);
}
static float home_start(uint8_t axis);

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Place the three objects for a normalised position.
 *
 * The bed's top face is a trapezoid, so a row's centre and half-width are
 * both linear in v; x is then just an offset from that row's centre. A true
 * projective transform would put a slight curve in v, but over 13 pixels of
 * depth that is well under half a pixel, so linear is the honest choice. */
static void scene_apply(const scene_pos_t * p) {
    const float front_cx = (SB_BED_FRONT_X0 + SB_BED_FRONT_X1) / 2.0f;
    const float back_cx  = (SB_BED_BACK_X0 + SB_BED_BACK_X1) / 2.0f;
    const float front_hw = (SB_BED_FRONT_X1 - SB_BED_FRONT_X0) / 2.0f;
    const float back_hw  = (SB_BED_BACK_X1 - SB_BED_BACK_X0) / 2.0f;

    float cx = front_cx + (back_cx - front_cx) * p->v;
    float hw = front_hw + (back_hw - front_hw) * p->v;
    float row_y = SB_BED_FRONT_Y + (SB_BED_BACK_Y - SB_BED_FRONT_Y) * p->v;

    // contact point: where the nozzle meets the bed, in the bed sprite's own
    // coordinates, and again on the screen
    int bed_x = (int)lroundf(cx + (p->u - 0.5f) * 2.0f * hw);
    int bed_y = (int)lroundf(row_y);
    int contact_x = SCENE_BED_X + bed_x;
    int contact_y = SCENE_BED_Y + bed_y;

    // depth foreshortening, shared by the head sprite and the Z lift
    float depth = hw / front_hw;

    // sb_head[0] is the far (smallest) sprite, so index runs backwards from v
    int idx = (int)lroundf((1.0f - p->v) * (SB_HEAD_COUNT - 1));
    if (idx < 0) idx = 0;
    if (idx >= SB_HEAD_COUNT) idx = SB_HEAD_COUNT - 1;
    const lv_img_dsc_t * sprite = sb_head[idx];

    float lift = SCENE_Z_MAX_PX *
                 (1.0f - expf(-(p->z * SCENE_Z_NEAR_PX_PER_MM) / SCENE_Z_MAX_PX));
    if (lift < 0) lift = 0;
    lift *= depth;

    /* Only re-point the image when the depth step actually changes. This runs
     * every animation frame, and lv_img_set_src() re-reads the header and
     * invalidates the object whether or not the source differs. */
    static const lv_img_dsc_t * shown = NULL;
    if (sprite != shown) {
        lv_img_set_src(scene_head, sprite);
        shown = sprite;
    }
    lv_obj_set_pos(scene_head,
                   contact_x - sprite->header.w / 2,
                   contact_y - (int)lroundf(lift) - sprite->header.h);

    /* Shadow tracks the head's width so it reads as cast by it, and fades as
     * the head climbs away from the bed. Positioned in bed-local coordinates
     * because it is a child of the bed image: near the bed's left or right
     * edge the ellipse would otherwise hang off onto the background and read
     * as a bite taken out of the bed, and being a child lets LVGL clip it. */
    int sw = (int)lroundf(sprite->header.w * 0.85f);
    int sh = sw / 3;
    if (sh < 3) sh = 3;
    lv_obj_set_size(scene_shadow, sw, sh);
    lv_obj_set_pos(scene_shadow, bed_x - sw / 2, bed_y - sh / 2);
    lv_opa_t opa = (lv_opa_t)clampf(150.0f - lift * 2.0f, 40.0f, 150.0f);
    static lv_opa_t shown_opa = 0;
    if (opa != shown_opa) {
        lv_obj_set_style_bg_opa(scene_shadow, opa, 0);
        shown_opa = opa;
    }
}

static void scene_anim_cb(void * var, int32_t t) {
    LV_UNUSED(var);
    float f = t / 1000.0f;
    pos_now.u = pos_from.u + (pos_to.u - pos_from.u) * f;
    pos_now.v = pos_from.v + (pos_to.v - pos_from.v) * f;
    pos_now.z = pos_from.z + (pos_to.z - pos_from.z) * f;
    scene_apply(&pos_now);
}

void lv_toolhead_scene_init(lv_obj_t * parent) {
    scene_bed = lv_img_create(parent);
    lv_img_set_src(scene_bed, &sb_bed);
    lv_obj_set_pos(scene_bed, SCENE_BED_X, SCENE_BED_Y);

    // child of the bed, so it is clipped to it -- see scene_apply()
    scene_shadow = lv_obj_create(scene_bed);
    lv_obj_remove_style_all(scene_shadow);
    lv_obj_clear_flag(scene_shadow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(scene_shadow, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(scene_shadow, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scene_shadow, 120, 0);

    scene_head = lv_img_create(parent);
    lv_img_set_src(scene_head, sb_head[SB_HEAD_COUNT - 1]);

    // names the state, under the bed; the emboss copy is created first so it
    // sits underneath, offset a pixel to thicken the stems
    scene_label_emboss = lv_label_create(parent);
    scene_label = lv_label_create(parent);
    for (lv_obj_t * l : {scene_label_emboss, scene_label}) {
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(SCENE_LABEL_COLOR), 0);
        lv_label_set_text(l, "");
    }
    scene_label_set("");

    lv_anim_init(&scene_anim);
    lv_anim_set_var(&scene_anim, scene_head);
    lv_anim_set_exec_cb(&scene_anim, scene_anim_cb);
    lv_anim_set_values(&scene_anim, 0, 1000);
    lv_anim_set_path_cb(&scene_anim, lv_anim_path_ease_out);

    scene_apply(&pos_now);
    /* Apply the hidden state directly rather than through
     * lv_toolhead_scene_set_active(): scene_active already starts false, so
     * that call would take its "no change" early return and leave the scene
     * on screen over the idle animation until the first probe ended. */
    scene_visibility(false);
}

/* The scene and the idle gif share ui_ScreenMainGif, so exactly one of them
 * is visible at a time. The shadow is a child of the bed and follows it.
 *
 * Hiding the gif object stops it being drawn but not decoded -- its frame
 * timer keeps running and keeps calling the GIF decoder, which is real work
 * for something nobody can see, on the one screen where the CPU is wanted
 * elsewhere. LVGL 8.3 has no lv_gif_pause() (added in 8.4), so the timer is
 * stopped through the widget struct, which lv_gif.h makes public for this. */
static void scene_gif_decode(bool run) {
    /* Only ever undo our own pause. lv_gif pauses this timer itself when a
     * finite-loop animation reaches its end, so resuming unconditionally
     * could restart one that had legitimately finished. It cannot reach that
     * end while we hold it paused, so this restores exactly the state we
     * interrupted. lv_gif_set_src() resumes the timer itself, which is
     * correct: a new source should play. */
    static bool paused_by_scene = false;
    lv_timer_t * t = ((lv_gif_t *)ui_img_main_gif)->timer;
    if (!t) return;   // no source set yet
    if (run && paused_by_scene) {
        lv_timer_resume(t);
        paused_by_scene = false;
    } else if (!run && !paused_by_scene) {
        lv_timer_pause(t);
        paused_by_scene = true;
    }
}

static void scene_visibility(bool active) {
    if (active) {
        lv_obj_clear_flag(scene_bed, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(scene_head, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(scene_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(scene_label_emboss, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_img_main_gif, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(scene_bed, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(scene_head, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(scene_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(scene_label_emboss, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_img_main_gif, LV_OBJ_FLAG_HIDDEN);
    }
    scene_gif_decode(!active);
}

void lv_toolhead_scene_set_mode(toolhead_scene_mode_t mode) {
    if (!scene_bed || mode == scene_mode) return;

    bool was_active = scene_active;
    scene_mode = mode;
    scene_active = (mode != TOOLHEAD_SCENE_OFF);

    if (scene_active) {
        scene_label_set(mode == TOOLHEAD_SCENE_HOMING  ? "Homing" :
                        mode == TOOLHEAD_SCENE_PROBING ? "Probing" : "QGL");
    }

    if (scene_active && !was_active) {
        last_sample_ms = 0;   // no stale easing from the last time it was up
        last_seq = 0;
        /* Open where the toolhead was last seen rather than at bed centre, so
         * the first frame is already about right and the first real sample
         * ~250ms later is a correction rather than a jump. Falls back to
         * centre when nothing has been seen yet. */
        pos_from = pos_to = pos_now = (scene_pos_t){
            home_start(0), home_start(1),
            moonraker.data.last_known_valid[2] ? moonraker.data.last_known[2]
                                               : SCENE_HOME_Z_MM };
        scene_apply(&pos_now);
    }
    /* Drop any ramp state on every entry to homing, not just when the scene
     * was off: probing -> homing keeps the scene up, and a latch left over
     * from a previous G28 would reconstruct against a stale origin. */
    if (mode == TOOLHEAD_SCENE_HOMING) home_reset();
    if (!scene_active) lv_anim_del(scene_head, scene_anim_cb);
    if (scene_active != was_active) scene_visibility(scene_active);
}

/* Per-axis reconstruction state for a homing move. */
typedef struct {
    bool  ramping;          // Klipper has force-set this axis and is homing it
    float origin;           // the force-set reading the ramp started from
    float start;            // where the toolhead really was at that moment
    float prev;             // last reading that DIFFERED, for snap detection
    uint32_t prev_change_ms;// when it differed, so speed is per change not per sample
    bool  have_prev;
} home_axis_t;
static home_axis_t home_axis[3];

static void home_reset(void) {
    memset(home_axis, 0, sizeof(home_axis));
}

static float home_speed(uint8_t axis) {
    const moonraker_data_t & d = moonraker.data;
    if (d.home_info.valid && d.home_info.speed[axis] > 0.0f)
        return d.home_info.speed[axis];
    return axis == 2 ? SCENE_HOME_FALLBACK_Z_SPEED : SCENE_HOME_FALLBACK_XY_SPEED;
}

/* Target while homing.
 *
 * Klipper reports which axes are homed, and this machine homes X, then Y,
 * then Z at bed centre. An axis that is already homed uses its real position;
 * the one in flight sweeps toward its endstop; the ones still queued hold
 * where they are. So the head is only ever guessing about the single axis
 * actually moving, and the guess is replaced by truth as soon as there is
 * any. */
/* Normalised start for an unhomed axis: where it was last seen, else centre. */
static float home_start(uint8_t axis) {
    const moonraker_data_t & d = moonraker.data;
    if (!d.last_known_valid[axis]) return 0.5f;
    float lo = d.axis_min[axis], hi = d.axis_max[axis];
    return clampf((d.last_known[axis] - lo) / (hi - lo), 0.0f, 1.0f);
}

/* Reconstruct where the toolhead really is during a homing move.
 *
 * Klipper does not report an unknown position while homing -- it reports a
 * fictitious one. To home an axis it force-sets the position to
 * `endstop -/+ 1.5 x travel`, well outside the axis range, then ramps toward
 * the endstop until the switch trips. Measured on this machine, homing X
 * jumps live_position to -183 and ramps to 351 over ~9s.
 *
 * That reading is not noise: it is offset from the truth by a constant, since
 * the ramp and the toolhead travel together. So the distance covered,
 * live - origin, is real, and adding it to where the head actually was
 * reconstructs where it actually is. No timed guess needed -- the animation
 * is driven by measured motion.
 *
 * Note homed_axes is useless here. It flips to "homed" when the force-set
 * happens, i.e. at the START of the move, and on a machine that was already
 * homed it never changes at all -- confirmed on hardware, it read "xyz"
 * unchanged through an entire G28 X. Relying on it is what made the head sit
 * pinned at the bed edge: the clamped -183 stayed put until the ramp climbed
 * back into range. */
static scene_pos_t homing_target(void) {
    const moonraker_data_t & d = moonraker.data;
    uint32_t now = millis();

    float out[3];
    for (uint8_t i = 0; i < 3; i++) {
        float lo = d.axis_min[i], hi = d.axis_max[i];
        float p = d.pos[i];
        home_axis_t * a = &home_axis[i];

        /* A speed no axis could physically reach means the endstop tripped and
         * Klipper replaced the fiction with the truth. From here the reading
         * is real -- which also matters for what follows, since G28 moves to
         * bed centre before homing Z.
         *
         * Only evaluated when the value actually changes, so the rate is real
         * regardless of how far ahead of Klipper's position cache we poll. */
        bool just_snapped = false;
        if (p != a->prev || !a->have_prev) {
            if (a->have_prev) {
                float gap = (now - a->prev_change_ms) / 1000.0f;
                if (gap < 0.001f) gap = 0.001f;
                if (a->ramping &&
                    fabsf(p - a->prev) / gap > home_speed(i) * SCENE_HOME_SNAP_FACTOR) {
                    a->ramping = false;
                    just_snapped = true;
                }
            }
            a->prev = p;
            a->prev_change_ms = now;
            a->have_prev = true;
        }
        /* Far enough outside its own limits to be Klipper's force-set
         * coordinate, which sits ~1.5x the axis travel out. The margin matters:
         * position_endstop equals position_max here, so the value snapped to on
         * a trigger lands a float-hair above the maximum and would otherwise be
         * read as a fresh force-set. Never on the sample that just snapped --
         * unlatching and relatching in one pass is always wrong. */
        if (!a->ramping && !just_snapped &&
            (p < lo - SCENE_HOME_FORCEPOS_MARGIN_MM ||
             p > hi + SCENE_HOME_FORCEPOS_MARGIN_MM)) {
            a->ramping = true;
            a->origin = p;
            a->start = d.last_known_valid[i] ? d.last_known[i] : (lo + hi) * 0.5f;
        }
        out[i] = a->ramping ? clampf(a->start + (p - a->origin), lo, hi) : p;
    }

    scene_pos_t r;
    r.u = clampf((out[0] - d.axis_min[0]) / (d.axis_max[0] - d.axis_min[0]), 0.0f, 1.0f);
    r.v = clampf((out[1] - d.axis_min[1]) / (d.axis_max[1] - d.axis_min[1]), 0.0f, 1.0f);
    /* Z reconstructs the same way -- the tap descends as far kinematically as
     * it does physically, so the nozzle is shown genuinely closing on the bed
     * rather than dropping on a timer. */
    r.z = out[2] < 0.0f ? 0.0f : out[2];
    return r;
}

void lv_toolhead_scene_update(void) {
    if (!scene_active) return;
    if (!moonraker.data.pos_valid || !moonraker.data.bounds_valid) return;

    /* Retarget once per fresh sample, not once per UI tick. The homing target
     * is a function of time, so value equality cannot tell a new sample from
     * the same one seen again 10ms later -- without this the easing would be
     * restarted continuously and never advance. */
    if (moonraker.data.seq == last_seq) return;
    last_seq = moonraker.data.seq;

    const float * lo = moonraker.data.axis_min;
    const float * hi = moonraker.data.axis_max;
    scene_pos_t sample = {
        .u = clampf((moonraker.data.pos[0] - lo[0]) / (hi[0] - lo[0]), 0.0f, 1.0f),
        .v = clampf((moonraker.data.pos[1] - lo[1]) / (hi[1] - lo[1]), 0.0f, 1.0f),
        .z = moonraker.data.pos[2] < 0.0f ? 0.0f : moonraker.data.pos[2],
    };
    if (scene_mode == TOOLHEAD_SCENE_HOMING) sample = homing_target();

    // ignore a repeat of the sample we are already heading for, so a stationary
    // toolhead does not restart the easing every cycle
    if (sample.u == pos_to.u && sample.v == pos_to.v && sample.z == pos_to.z) return;

    uint32_t now = millis();
    uint32_t interval = last_sample_ms ? (now - last_sample_ms) : SCENE_LERP_MIN_MS;
    last_sample_ms = now;
    if (interval < SCENE_LERP_MIN_MS) interval = SCENE_LERP_MIN_MS;
    if (interval > SCENE_LERP_MAX_MS) interval = SCENE_LERP_MAX_MS;

    lv_anim_del(scene_head, scene_anim_cb);
    pos_from = pos_now;
    pos_to = sample;
    lv_anim_set_time(&scene_anim, interval);
    lv_anim_start(&scene_anim);
}
