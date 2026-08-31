#ifndef MOONRAKER_H
#define MOONRAKER_H

#include <WString.h>

// typedef enum {
//     MOONRAKER_STATE_HOMING = 0,
//     MOONRAKER_STATE_PROBING,
//     MOONRAKER_STATE_QGLING,
//     MOONRAKER_STATE_NOZZLE_HEATING,
//     MOONRAKER_STATE_BED_HEATING,
//     MOONRAKER_STATE_PRINTING,
//     MOONRAKER_STATE_IDLE,
// } moonraker_status_t;

typedef struct {
    int16_t bed_actual;
    int16_t bed_target;
    int16_t nozzle_actual;
    int16_t nozzle_target;
    uint8_t progress;
    char file_path[32];

    /* Live toolhead position and the bed's own extents, both in mm.
     *
     * pos comes from motion_report.live_position, which is where the toolhead
     * actually is right now, not toolhead.position (the last *commanded*
     * point, which runs ahead of real motion while a move is queued).
     *
     * axis_min/axis_max are static for a given printer but are read from it
     * rather than assumed, so the scene is correct on any bed size. They stay
     * invalid until the first successful query. */
    float pos[3];
    float axis_min[3];
    float axis_max[3];
    bool pos_valid;
    bool bounds_valid;

    /* Which axes Klipper considers homed, e.g. "" / "x" / "xy" / "xyz".
     * Position for an axis not listed here is meaningless, which is the whole
     * problem the homing animation exists to paper over. */
    char homed_axes[8];

    /* Last position each axis was seen at while it was homed, kept per axis.
     *
     * A Klipper restart forgets where the toolhead is; the toolhead does not
     * move because of it. Holding the last trustworthy reading here means the
     * homing animation can start from roughly the truth instead of from the
     * middle of the bed. Survives a Klipper or Moonraker restart, not a KNOMI
     * reboot -- after that the head may have been moved by hand anyway, and
     * bed centre is the honest answer. */
    float last_known[3];
    bool last_known_valid[3];

    /* Published by the _KNOMI_HOME_INFO macro in voron_knomi.cfg, which
     * derives them from the live printer.cfg at startup. Absent on a printer
     * without that macro, in which case the scene uses compiled-in defaults. */
    struct {
        bool valid;
        float home[2];    // where X and Y land when homed, mm
        float speed[3];   // homing_speed per axis, mm/s
    } home_info;

    /* Bumped on every successful status query. The scene retargets on this
     * rather than on the position changing: during homing its target is a
     * function of time, so "has the value changed" cannot distinguish a fresh
     * sample from the same one seen again at UI rate. */
    uint32_t seq;

    bool pause;
    bool printing;    // is klipper in a printing task (including printing, pausing, paused, cancelling)
    bool homing;
    bool probing;
    bool qgling;
    bool heating_nozzle;
    bool heating_bed;
} moonraker_data_t;

#define QUEUE_LEN 5
typedef struct {
    String queue[QUEUE_LEN];
    uint8_t index_r;  // Ring buffer read position
    uint8_t index_w;  // Ring buffer write position
    uint8_t count;    // Count of commands in the queue
} post_queue_t;

class MOONRAKER {
    public:
        bool unconnected;   // is KNOMI connected to moonraker
        bool unready; // is moonraker connected to klipper
        bool data_unlock; //
        moonraker_data_t data;
        void http_get_loop(void);
        void http_post_loop(void);
        bool post_to_queue(String path);
        bool post_gcode_to_queue(String gcode);
        String send_request(const char * type, String path);
        // keep-alive variant, safe only from moonraker_task -- see moonraker.cpp
        String request(const char * type, String path, bool keepalive);

    private:
        post_queue_t post_queue;
        void get_printer_ready(void);
        void get_printer_info(void);
        void get_progress(void);
        void get_status_and_position(void);
};

extern MOONRAKER moonraker;

#endif
