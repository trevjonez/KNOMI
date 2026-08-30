#ifndef CONFIG_H
#define CONFIG_H

#define FW_VERSION "V1.0.2"

 // default 80 for http
#define SERVER_PORT 80

#define SCAN_SSIDS_NUM 10     //  The max number of SSIDs that we will scan for.

#define HOSTNAME "KNOMI"
#define AP_SSID "BTT-KNOMI" // Create a SSID for BTT KNOMI Access Point
#define AP_PWD "" // Default no password
#define AP_LOCAL_IP IPAddress(192, 168, 20, 1) // access point IP
#define AP_GATEWAY  IPAddress(192, 168, 20, 1) // gateway IP
#define AP_SUBNET   IPAddress(255, 255, 255, 0) // subnet mask

#define WIFI_STA_TIMEOUT 15000  // 15s
// STA reconnect backoff, see the retry block in wifi_task()
#define WIFI_STA_RETRY_MIN_MS 10000   // first retry 10s after a failure
#define WIFI_STA_RETRY_MAX_MS 60000   // doubles each attempt, capped at 60s
// consecutive failed joins before falling back to the AP config portal, so a
// bad password is still recoverable without a USB reflash
#define WIFI_STA_MAX_FAILS 5

/* Upper bound on screen transition animations, in ms.
 *
 * Every _ui_screen_change() call site (~40 of them, nearly all in the
 * SquareLine-generated ui.c) hardcodes 500ms, which is slow enough to be the
 * dominant part of how a swipe feels. Rather than editing generated files,
 * _ui_screen_change() clamps to this -- one place to tune, and it survives
 * regenerating ui.c. Call sites that pass 0 stay instant.
 *
 * Shorter also means less work: a slide transition redraws the whole screen
 * every frame, so this is the most render-heavy thing the device does. */
#define UI_SCREEN_ANIM_MS 180

// BTT red color for UI (RGB888)
#define LV_32BIT_BTT_RED    0xC02F30
#define LV_32BIT_BTT_BLUE   0x209ADE
#define LV_32BIT_BTT_PURPLE 0xA91DDA
#define LV_32BIT_BTT_GREEN  0x5DA910
#define LV_DEFAULT_COLOR    LV_32BIT_BTT_RED

#endif
