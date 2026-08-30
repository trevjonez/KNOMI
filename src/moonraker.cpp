#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "moonraker.h"
#include "knomi.h"

// #define MOONRAKER_DEBUG

void lv_popup_warning(const char * warning, bool clickable);
void lv_roller_fetch_pending(void);

String MOONRAKER::send_request(const char * type, String path) {
    String ip = knomi_config.moonraker_ip;
    String port = knomi_config.moonraker_port;
    String url = "http://" + ip + ":" + port + path;
    String response = "";
    HTTPClient client;
    // replace all " " space to "%20" for http
    url.replace(" ", "%20");
    client.begin(url);
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
    String webhooks = send_request("GET", "/printer/objects/query?webhooks");
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
    String printer_info = send_request("GET", "/api/printer");
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
    String display_status = send_request("GET", "/printer/objects/query?virtual_sdcard");
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
 * _KNOMI_STATUS query left the round trip unchanged at ~200-250ms. So the
 * toolhead scene gets its position for free rather than needing a poll of
 * its own, and position stays in step with the flags that select the screen.
 *
 * Field selection (=live_position, =axis_minimum,...) matters: a bare
 * motion_report also returns the stepper and trapq name lists, which are
 * several hundred bytes of JSON this never looks at. */
void MOONRAKER::get_status_and_position(void) {
    String knomi_status = send_request("GET",
        "/printer/objects/query?gcode_macro%20_KNOMI_STATUS"
        "&motion_report=live_position"
        "&toolhead=axis_minimum,axis_maximum");
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
 * full ~200ms round trip against Moonraker. Three requests per cycle gives the
 * scene ~1.2Hz, which steps visibly; one gives ~4Hz, which tracks.
 *
 * Nothing is lost by skipping get_printer_ready() here -- a failed GET still
 * sets unconnected from inside send_request(), and the state flags that end
 * this branch come from the one query that is still being made. */
static inline bool live_scene_state(const moonraker_data_t & d) {
    return d.probing || d.qgling;
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
        // (the request itself already costs ~200ms); everything else is fine at
        // the original cadence.
        delay(live_scene_state(moonraker.data) ? 20 : 200);
    }
}

// Klipper Control: Restart Firmware Restart. /printer/restart, /printer/firmware_restart
// Service Control: stop start restart. POST /machine/services/stop|restart|start?service={name}
// Host Control: Reboot, Shutdown. POST /machine/shutdown, POST /machine/reboot
