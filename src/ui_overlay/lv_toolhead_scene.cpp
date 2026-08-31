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

/* Homing sweep.
 *
 * While an axis is unhomed the printer does not know where the toolhead is,
 * so neither do we. Two things narrow the guess to something usually true:
 *
 *   where it starts   moonraker.data.last_known -- the last position each
 *                     axis was seen at while homed. A Klipper restart forgets
 *                     the position but does not move the toolhead, so this is
 *                     still right afterwards. Falls back to bed centre.
 *   where it is going and how fast: the _KNOMI_HOME_INFO macro publishes
 *                     position_endstop and homing_speed straight out of the
 *                     live printer.cfg, so editing that config is enough --
 *                     no reflash. Falls back to the constants below.
 *
 * With both, the sweep is not an estimate at all: duration is the distance
 * actually to be travelled over the actual homing speed. And it never has to
 * be right, because the moment Klipper reports an axis homed that axis
 * switches to real position and eases into it -- guess long and it lands
 * early, guess short and it waits at the endstop. */

/* Used only until _KNOMI_HOME_INFO answers. Values for a Voron 2.4, whose X
 * and Y both home to maximum (position_endstop == position_max). */
#define SCENE_HOME_FALLBACK_XY_SPEED  40.0f   // mm/s
#define SCENE_HOME_FALLBACK_Z_SPEED    8.0f   // mm/s
#define SCENE_HOME_FALLBACK_TARGET_U   1.0f   // 1 = toward max, 0 = toward min
#define SCENE_HOME_FALLBACK_TARGET_V   1.0f

/* Where the head floats while Z is still unknown, mm. Also the height the Z
 * sweep descends from, so it doubles as the distance that sweep represents. */
#define SCENE_HOME_Z_MM         15.0f

/* Bounds on a computed sweep, ms. A very short home move should still be
 * visible as motion; a very long one should not outlast anyone's patience. */
#define SCENE_HOME_SWEEP_MIN_MS  400
#define SCENE_HOME_SWEEP_MAX_MS  12000

typedef struct {
    float u;    // 0..1 across the bed, left to right
    float v;    // 0..1 front to back
    float z;    // mm above the bed
} scene_pos_t;

static lv_obj_t * scene_bed = NULL;
static lv_obj_t * scene_shadow = NULL;
static lv_obj_t * scene_head = NULL;
static lv_obj_t * scene_label = NULL;
static lv_anim_t scene_anim;
static toolhead_scene_mode_t scene_mode = TOOLHEAD_SCENE_OFF;

static scene_pos_t pos_from = {0.5f, 0.5f, 0.0f};
static scene_pos_t pos_to   = {0.5f, 0.5f, 0.0f};
static scene_pos_t pos_now  = {0.5f, 0.5f, 0.0f};

static bool scene_active = false;
static uint32_t last_sample_ms = 0;
static uint32_t last_seq = 0;

// homing sweep state: which axis is in flight, and when that phase started
static uint8_t home_phase = 0;
static uint32_t home_phase_ms = 0;

static void scene_visibility(bool active);
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

    // names the state, under the bed
    scene_label = lv_label_create(parent);
    lv_obj_set_style_text_font(scene_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(scene_label, lv_color_white(), 0);
    lv_label_set_text(scene_label, "");
    lv_obj_align(scene_label, LV_ALIGN_TOP_MID, 0,
                 SCENE_BED_Y + SB_BED_H + SCENE_LABEL_GAP);

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
        lv_obj_add_flag(ui_img_main_gif, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(scene_bed, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(scene_head, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(scene_label, LV_OBJ_FLAG_HIDDEN);
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
        lv_label_set_text(scene_label,
            mode == TOOLHEAD_SCENE_HOMING  ? "Homing" :
            mode == TOOLHEAD_SCENE_PROBING ? "Probing" : "QGL");
        lv_obj_align(scene_label, LV_ALIGN_TOP_MID, 0,
                     SCENE_BED_Y + SB_BED_H + SCENE_LABEL_GAP);
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
    /* Invalidate the sweep clock on every entry to homing, not just when the
     * scene was off: probing -> homing keeps the scene up, and a stale
     * home_phase_ms there would start the sweep already finished. */
    if (mode == TOOLHEAD_SCENE_HOMING) home_phase = 0xFF;
    if (!scene_active) lv_anim_del(scene_head, scene_anim_cb);
    if (scene_active != was_active) scene_visibility(scene_active);
}

/* Linear ramp from a to b over span_ms, holding at b once it arrives. Linear
 * rather than eased because it stands in for a homing move, which runs at a
 * constant speed. */
static float sweep(float a, float b, uint32_t elapsed, uint32_t span_ms) {
    if (elapsed >= span_ms) return b;
    return a + (b - a) * ((float)elapsed / (float)span_ms);
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

/* Normalised endstop for an unhomed axis, from the printer if it says. */
static float home_end(uint8_t axis) {
    const moonraker_data_t & d = moonraker.data;
    if (!d.home_info.valid)
        return axis == 0 ? SCENE_HOME_FALLBACK_TARGET_U : SCENE_HOME_FALLBACK_TARGET_V;
    float lo = d.axis_min[axis], hi = d.axis_max[axis];
    return clampf((d.home_info.home[axis] - lo) / (hi - lo), 0.0f, 1.0f);
}

/* How long a move of mm at this axis's homing speed takes. */
static uint32_t home_span(uint8_t axis, float mm) {
    const moonraker_data_t & d = moonraker.data;
    float speed = d.home_info.valid ? d.home_info.speed[axis]
                : (axis == 2 ? SCENE_HOME_FALLBACK_Z_SPEED
                             : SCENE_HOME_FALLBACK_XY_SPEED);
    if (speed <= 0.0f) speed = SCENE_HOME_FALLBACK_XY_SPEED;
    float ms = fabsf(mm) / speed * 1000.0f;
    if (ms < SCENE_HOME_SWEEP_MIN_MS) ms = SCENE_HOME_SWEEP_MIN_MS;
    if (ms > SCENE_HOME_SWEEP_MAX_MS) ms = SCENE_HOME_SWEEP_MAX_MS;
    return (uint32_t)ms;
}

/* Sweep one normalised axis from where it was last seen to its endstop, paced
 * by the real distance between them at the real homing speed. */
static float home_sweep(uint8_t axis, uint32_t t) {
    float a = home_start(axis), b = home_end(axis);
    float span_mm = (b - a) * (moonraker.data.axis_max[axis] - moonraker.data.axis_min[axis]);
    return sweep(a, b, t, home_span(axis, span_mm));
}

static scene_pos_t homing_target(const scene_pos_t * live) {
    const char * ha = moonraker.data.homed_axes;
    bool has_x = strchr(ha, 'x') != NULL;
    bool has_y = strchr(ha, 'y') != NULL;
    bool has_z = strchr(ha, 'z') != NULL;

    uint8_t phase = !has_x ? 0 : (!has_y ? 1 : 2);
    if (phase != home_phase) {
        home_phase = phase;
        home_phase_ms = millis();
    }
    uint32_t t = millis() - home_phase_ms;

    scene_pos_t p;
    p.u = has_x ? live->u : home_sweep(0, t);
    p.v = has_y ? live->v : (has_x ? home_sweep(1, t) : home_start(1));
    p.z = has_z ? live->z
                : (has_y ? sweep(SCENE_HOME_Z_MM, 0.0f, t,
                                 home_span(2, SCENE_HOME_Z_MM))
                         : SCENE_HOME_Z_MM);
    return p;
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
    if (scene_mode == TOOLHEAD_SCENE_HOMING) sample = homing_target(&sample);

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
