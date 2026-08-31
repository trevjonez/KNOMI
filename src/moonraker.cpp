#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "moonraker.h"
#include "knomi.h"

// #define MOONRAKER_DEBUG

void lv_popup_warning(const char * warning, bool clickable);
void lv_roller_fetch_pending(void);

/* A connection held open across polls, for the status path only.
 *
 * Every request used to build its own HTTPClient, so every sample paid a TCP
 * handshake. That was invisible while Klipper's 250ms status tick dominated
 * the round trip, but once the tick is shortened (see _KNOMI_QUERY_RATE in
 * voron_knomi.cfg) the handshake becomes the limiting cost -- and it also
 * wakes the radio for a full exchange the poll does not need.
 *
 * Only the status path uses it, and that path lives entirely in
 * moonraker_task. POSTs run on moonraker_post_task and keep their own
 * short-lived client: one HTTPClient cannot be driven from two tasks.
 *
 * Note this is never end()ed on the happy path. HTTPClient::end() closes the
 * socket regardless of setReuse(), so calling it is exactly what defeats
 * keep-alive. It IS called on failure, so a printer that went away cannot
 * leave a wedged socket behind. */
static WiFiClient keepalive_tcp;
static HTTPClient keepalive_http;

String MOONRAKER::send_request(const char * type, String path) {
    return request(type, path, false);
}

String MOONRAKER::request(const char * type, String path, bool keepalive) {
    String ip = knomi_config.moonraker_ip;
    String port = knomi_config.moonraker_port;
    String url = "http://" + ip + ":" + port + path;
    String response = "";
    HTTPClient local;
    HTTPClient & client = keepalive ? keepalive_http : local;
    // replace all " " space to "%20" for http
    url.replace(" ", "%20");
    if (keepalive) {
        client.setReuse(true);
        client.begin(keepalive_tcp, url);
    } else {
        client.begin(url);
    }
    // Bound the TCP connect separately from the read. Without this a powered-off
    // printer stalls the caller for the arduino-esp32 default connect timeout,
    // and the 60s read timeout below applies to that wait too.
    client.setConnectTimeout(1000);
    // set timeout to 60 seconds since some gcode like G28 need long time to feedback
    client.setTimeout(60000);
    int code = client.sendRequest(type, "");
    // http request success
    if (code > 0) {
        unconnected = false;
        response = client.getString();
        if (code == 400) {
            if (!response.isEmpty()) {
                // Serial.println(response.c_str());
                DynamicJsonDocument json_parse(response.length() * 2);
                deserializeJson(json_parse, response);
                String msg = json_parse["error"]["message"].as<String>();
#ifdef MOONRAKER_DEBUG
                Serial.println(msg.c_str());
#endif
                msg.remove(0, 41); //  remove header {'error': 'WebRequestError', 'message':
                msg.remove(msg.length() - 2, 2); // remove tail }
                msg.replace("\\n", "\n");
                lv_popup_warning(msg.c_str(), true);
            }
        }
    } else {
        /*
         * since some gcode need long time cause code=-11 error
         * so don't set status when POST gcode
         * only set when GET
         */
        if (strcmp(type, "GET") == 0)
            unconnected = true;
        Serial.printf("moonraker http %s error.\r\n", type);
    }
    /* end() closes the socket whatever setReuse() says, so on the keep-alive
     * path it is only for tearing a broken connection down -- the next request
     * then reconnects cleanly. Calling it after a good response would undo the
     * whole point. */
    if (!keepalive || code <= 0)
        client.end(); //Free the resources

#ifdef MOONRAKER_DEBUG
    Serial.printf("\r\n\r\n %s code:%d************ %s *******************\r\n\r\n", type, code, url.c_str());
    Serial.println(response.c_str());
    Serial.println("\r\n*******************************\r\n\r\n");
#endif

    return response;
}

void MOONRAKER::http_post_loop(void) {
    if (post_queue.count == 0) return;
    send_request("POST", post_queue.queue[post_queue.index_r]);
    post_queue.count--;
    post_queue.index_r = (post_queue.index_r + 1) % QUEUE_LEN;
}

bool MOONRAKER::post_to_queue(String path) {
    if (post_queue.count >= QUEUE_LEN) {
        Serial.println("moonraker post queue overflow!");
        return false;
    }
    post_queue.queue[post_queue.index_w] = path;
    post_queue.index_w = (post_queue.index_w + 1) % QUEUE_LEN;
    post_queue.count++;
#ifdef MOONRAKER_DEBUG
    Serial.printf("\r\n\r\n ************ post queue *******************\r\n\r\n");
    Serial.print("count: ");   Serial.println(post_queue.count);
    Serial.print("index_w: "); Serial.println(post_queue.index_w);
    Serial.print("queue: ");   Serial.println(path);
    Serial.println("\r\n*******************************\r\n\r\n");
#endif
    return true;
}

bool MOONRAKER::post_gcode_to_queue(String gcode) {
    String path = "/printer/gcode/script?script=" + gcode;
    return post_to_queue(path);
}

void MOONRAKER::get_printer_ready(void) {
    String webhooks = request("GET", "/printer/objects/query?webhooks", true);
    if (!webhooks.isEmpty()) {
        DynamicJsonDocument json_parse(webhooks.length() * 2);
        deserializeJson(json_parse, webhooks);
        String state = json_parse["result"]["status"]["webhooks"]["state"].as<String>();
        unready = (state == "ready") ? false : true;
#ifdef MOONRAKER_DEBUG
        Serial.print("unready: ");
        Serial.println(unready);
#endif
    } else {
        unready = true;
        Serial.println("Empty: moonraker: get_printer_ready");
    }
}

void MOONRAKER::get_printer_info(void) {
    String printer_info = request("GET", "/api/printer", true);
    if (!printer_info.isEmpty()) {
        DynamicJsonDocument json_parse(printer_info.length() * 2);
        deserializeJson(json_parse, printer_info);
        data.pause = json_parse["state"]["flags"]["pausing"].as<bool>(); // pausing
        data.pause |= json_parse["state"]["flags"]["paused"].as<bool>(); // paused
        data.printing = json_parse["state"]["flags"]["printing"].as<bool>(); // printing
        data.printing |= json_parse["state"]["flags"]["cancelling"].as<bool>(); // cancelling
        data.printing |= data.pause; // pause
        data.bed_actual = int16_t(json_parse["temperature"]["bed"]["actual"].as<double>() + 0.5f);
        data.bed_target = int16_t(json_parse["temperature"]["bed"]["target"].as<double>() + 0.5f);
        data.nozzle_actual = int16_t(json_parse["temperature"][knomi_config.moonraker_tool]["actual"].as<double>() + 0.5f);
        data.nozzle_target = int16_t(json_parse["temperature"][knomi_config.moonraker_tool]["target"].as<double>() + 0.5f);
#ifdef MOONRAKER_DEBUG
        Serial.print("unoperational: ");
        Serial.println(unoperational);
        Serial.print("printing: ");
        Serial.println(data.printing);
        Serial.print("bed_actual: ");
        Serial.println(data.bed_actual);
        Serial.print("bed_target: ");
        Serial.println(data.bed_target);
        Serial.print("nozzle_actual: ");
        Serial.println(data.nozzle_actual);
        Serial.print("nozzle_target: ");
        Serial.println(data.nozzle_target);
#endif
    } else {
        Serial.println("Empty: moonraker: get_printer_info");
    }
}

// only return gcode file name except path
// for example:"SD:/test/123.gcode"
// only return "123.gcode"
const char * path_only_gcode(const char * path)
{
  char * name = strrchr(path, '/');

  if (name != NULL)
    return (name + 1);
  else
    return path;
}

void MOONRAKER::get_progress(void) {
    String display_status = request("GET", "/printer/objects/query?virtual_sdcard", true);
    if (!display_status.isEmpty()) {
        DynamicJsonDocument json_parse(display_status.length() * 2);
        deserializeJson(json_parse, display_status);
        data.progress = (uint8_t)(json_parse["result"]["status"]["virtual_sdcard"]["progress"].as<double>() * 100 + 0.5f);
        String path = json_parse["result"]["status"]["virtual_sdcard"]["file_path"].as<String>();
        strlcpy(data.file_path, path_only_gcode(path.c_str()), sizeof(data.file_path) - 1);
        data.file_path[sizeof(data.file_path) - 1] = 0;
#ifdef MOONRAKER_DEBUG
        Serial.print("progress: ");
        Serial.println(data.progress);
        Serial.print("path: ");
        Serial.println(data.file_path);
#endif
    } else {
        Serial.println("Empty: moonraker: get_progress");
    }
}

/* Status flags and live toolhead position, in a single request.
 *
 * Klipper answers a multi-object query for the same cost as a single one --
 * measured on this printer, adding motion_report and the axis limits to the
 * _KNOMI_STATUS query left the round trip unchanged at ~250ms. So the
 * toolhead scene gets its position for free rather than needing a poll of
 * its own, and position stays in step with the flags that select the screen.
 *
 * That is not a coincidence. objects/query does not run when it arrives: it
 * queues and waits for Klipper's subscription tick (SUBSCRIPTION_REFRESH_TIME
 * = .25 in klippy/webhooks.py), and one tick serves every waiting client and
 * every object in one pass, calling get_status() once per object however many
 * asked. So the cost is per REQUEST, not per object -- which is why bundling
 * is free and why sequential requests are so expensive.
 *
 * Field selection (=live_position, =axis_minimum,...) matters: a bare
 * motion_report also returns the stepper and trapq name lists, which are
 * several hundred bytes of JSON this never looks at. */
void MOONRAKER::get_status_and_position(void) {
    String knomi_status = request("GET",
        "/printer/objects/query?gcode_macro%20_KNOMI_STATUS"
        "&motion_report=live_position"
        "&toolhead=axis_minimum,axis_maximum,homed_axes"
        "&gcode_macro%20_KNOMI_HOME_INFO", true);
    if (!knomi_status.isEmpty()) {
        DynamicJsonDocument json_parse(knomi_status.length() * 2);
        deserializeJson(json_parse, knomi_status);

        JsonVariant live = json_parse["result"]["status"]["motion_report"]["live_position"];
        if (live.is<JsonArray>() && live.size() >= 3) {
            for (uint8_t i = 0; i < 3; i++)
                data.pos[i] = live[i].as<float>();
            data.pos_valid = true;
        }
        JsonVariant amin = json_parse["result"]["status"]["toolhead"]["axis_minimum"];
        JsonVariant amax = json_parse["result"]["status"]["toolhead"]["axis_maximum"];
        if (amin.is<JsonArray>() && amin.size() >= 3 && amax.is<JsonArray>() && amax.size() >= 3) {
            for (uint8_t i = 0; i < 3; i++) {
                data.axis_min[i] = amin[i].as<float>();
                data.axis_max[i] = amax[i].as<float>();
            }
            // guard against a degenerate bed that would divide by zero downstream
            data.bounds_valid = (data.axis_max[0] > data.axis_min[0]) &&
                                (data.axis_max[1] > data.axis_min[1]);
        }
        String homed = json_parse["result"]["status"]["toolhead"]["homed_axes"].as<String>();
        if (homed == "null") homed = "";
        strlcpy(data.homed_axes, homed.c_str(), sizeof(data.homed_axes));

        /* Remember where each axis was while Klipper still vouched for it.
         * Recorded here rather than in the scene because it has to keep
         * accruing while the scene is off -- idle is exactly when the
         * position is known and worth keeping.
         *
         * Never during a G28. homed_axes cannot be used to tell a real
         * position from Klipper's homing fiction: it keeps claiming the axis
         * is homed throughout the move, and on a machine that was already
         * homed it does not change at all. Recording then would overwrite the
         * one value the homing animation needs -- where the head was before
         * the move began -- with the force-set coordinate, which collapses
         * the reconstruction back to the raw reading.
         *
         * Read from this sample's JSON rather than data.homing, which still
         * holds the previous cycle's value at this point.
         *
         * The range test is a second guard: a force-set coordinate always
         * lands outside the axis's own limits. */
        bool homing_now = json_parse["result"]["status"]
                                    ["gcode_macro _KNOMI_STATUS"]["homing"].as<bool>();
        if (data.pos_valid && data.bounds_valid && !homing_now) {
            const char * axis = "xyz";
            for (uint8_t i = 0; i < 3; i++) {
                if (strchr(data.homed_axes, axis[i]) &&
                    data.pos[i] >= data.axis_min[i] &&
                    data.pos[i] <= data.axis_max[i]) {
                    data.last_known[i] = data.pos[i];
                    data.last_known_valid[i] = true;
                }
            }
        }

        JsonVariant hi = json_parse["result"]["status"]["gcode_macro _KNOMI_HOME_INFO"];
        if (hi["ready"].as<bool>()) {
            data.home_info.home[0] = hi["x_home"].as<float>();
            data.home_info.home[1] = hi["y_home"].as<float>();
            data.home_info.speed[0] = hi["x_speed"].as<float>();
            data.home_info.speed[1] = hi["y_speed"].as<float>();
            data.home_info.speed[2] = hi["z_speed"].as<float>();
            // a zero speed would divide by zero downstream; treat it as absent
            data.home_info.valid = data.home_info.speed[0] > 0.0f &&
                                   data.home_info.speed[1] > 0.0f &&
                                   data.home_info.speed[2] > 0.0f;
        }
        data.seq++;

        data.homing = json_parse["result"]["status"]["gcode_macro _KNOMI_STATUS"]["homing"].as<bool>();
        data.probing = json_parse["result"]["status"]["gcode_macro _KNOMI_STATUS"]["probing"].as<bool>();
        data.qgling = json_parse["result"]["status"]["gcode_macro _KNOMI_STATUS"]["qgling"].as<bool>();
        data.heating_nozzle = json_parse["result"]["status"]["gcode_macro _KNOMI_STATUS"]["heating_nozzle"].as<bool>();
        data.heating_bed = json_parse["result"]["status"]["gcode_macro _KNOMI_STATUS"]["heating_bed"].as<bool>();
#ifdef MOONRAKER_DEBUG
        Serial.print("homing: ");
        Serial.println(data.homing);
        Serial.print("probing: ");
        Serial.println(data.probing);
        Serial.print("qgling: ");
        Serial.println(data.qgling);
        Serial.print("heating_nozzle: ");
        Serial.println(data.heating_nozzle);
        Serial.print("heating_bed: ");
        Serial.println(data.heating_bed);
#endif
    } else {
        Serial.println("Empty: moonraker: get_status_and_position");
    }
}

/* While the live toolhead scene is on screen, position is the only thing
 * displayed that changes, so drop everything else out of the cycle: the ready
 * flag and the temperatures are not on that screen, and each extra query is a
 * full ~250ms round trip. Three requests per cycle gives the scene ~1.2Hz,
 * which steps visibly; one gives ~4Hz, which tracks.
 *
 * The 250ms is Klipper's status refresh period, not network or Moonraker
 * latency (Moonraker's own endpoints answer in ~6ms, and querying klippy.sock
 * directly is identical). Each request waits for the next tick, so N
 * sequential requests cost N x 250ms no matter how little each one asks for.
 * 4Hz is the ceiling for everyone; polling harder gains nothing.
 *
 * Nothing is lost by skipping get_printer_ready() here -- a failed GET still
 * sets unconnected from inside send_request(), and the state flags that end
 * this branch come from the one query that is still being made. */
static inline bool live_scene_state(const moonraker_data_t & d) {
    return d.homing || d.probing || d.qgling;
}

void MOONRAKER::http_get_loop(void) {
    data_unlock = false;
    if (live_scene_state(data)) {
        get_status_and_position();
        data_unlock = true;
        return;
    }
    get_printer_ready();
    if (!unready) {
        // get_status_and_position() must before get_printer_info()
        // avoid homing, qgling, etc action flag = 1
        // but printing flag has not refresh
        get_status_and_position();
        get_printer_info();
        if (data.printing) {
            get_progress();
        }
    }
    data_unlock = true;
}

MOONRAKER moonraker;

void moonraker_post_task(void * parameter) {
    for(;;) {
        moonraker.http_post_loop();
        delay(500);
    }
}

void moonraker_task(void * parameter) {

    xTaskCreate(moonraker_post_task, "moonraker post",
        4096,  // Stack size (bytes)
        NULL,  // Parameter to pass
        8,     // Task priority
        NULL   // Task handle
        );

    for(;;) {
        if (wifi_get_connect_status() == WIFI_STATUS_CONNECTED) {
            moonraker.http_get_loop();
            // Roller list fetches, moved off the UI task so a slow or absent
            // printer can no longer freeze the screen.
            lv_roller_fetch_pending();
        }
        // The toolhead scene wants position as fast as the printer will answer
        // (the request itself already costs ~250ms); everything else is fine at
        // the original cadence.
        delay(live_scene_state(moonraker.data) ? 20 : 200);
    }
}

// Klipper Control: Restart Firmware Restart. /printer/restart, /printer/firmware_restart
// Service Control: stop start restart. POST /machine/services/stop|restart|start?service={name}
// Host Control: Reboot, Shutdown. POST /machine/shutdown, POST /machine/reboot
