#include "lvgl_hal.h"
#include "pinout.h"
#include <esp_heap_caps.h>   // internal/DMA-capable draw buffers

TFT_eSPI tft_gc9a01 = TFT_eSPI();
#ifdef CST816S_SUPPORT
extern TwoWire i2c0;
CST816S ts_cst816s = CST816S(CST816S_RST_PIN, CST816S_IRQ_PIN, &i2c0);
#endif

/* Display flushing */
void usr_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    // usr fill color
    tft_gc9a01.startWrite();
    tft_gc9a01.setAddrWindow(area->x1, area->y1, w, h);
    tft_gc9a01.pushColors((uint16_t *)&color_p->full, w * h, true);
    tft_gc9a01.endWrite();

    lv_disp_flush_ready(disp);
}

#ifdef CST816S_SUPPORT
void touch_idle_time_clear(void);
void usr_touchpad_read(struct _lv_indev_drv_t * indev_drv, lv_indev_data_t * data) {
    static touch_event_t event;
    if(ts_cst816s.ready()) {
        ts_cst816s.getTouch(&event);
    }
    if(event.finger) {
        data->state = LV_INDEV_STATE_PR;
        /*Set the coordinates*/
        data->point.x = event.x;
        data->point.y = event.y;
        touch_idle_time_clear();
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}
#endif

static int8_t aw9346_from_light = -1;

void tft_backlight_init(void) {
    pinMode(LCD_BL_PIN, OUTPUT);
    digitalWrite(LCD_BL_PIN, LOW);
    delay(3); // > 2.5ms for shutdown
    aw9346_from_light = 0;
}
void tft_set_backlight(int8_t aw9346_to_light) {
    if (aw9346_to_light > 16) aw9346_to_light = 16;
    if (aw9346_to_light < 0) aw9346_to_light = 0;
    if (aw9346_from_light == aw9346_to_light) return;

    if (aw9346_to_light == 0) {
        digitalWrite(LCD_BL_PIN, LOW);
        delay(3); // > 2.5ms for shutdown
        aw9346_from_light = 0;
        return;
    }
    if (aw9346_from_light <= 0) {
        digitalWrite(LCD_BL_PIN, HIGH);
        delayMicroseconds(25); // > 20us for poweron
        aw9346_from_light = 16;
    }

    if (aw9346_from_light < aw9346_to_light)
        aw9346_from_light += 16;

    int8_t num = aw9346_from_light - aw9346_to_light;

    for (int8_t i = 0; i < num; i++) {
        digitalWrite(LCD_BL_PIN, LOW);
        delayMicroseconds(1); // 0.5us < T_low < 500us
        digitalWrite(LCD_BL_PIN, HIGH);
        delayMicroseconds(1); // 0.5us < T_high
    }

    aw9346_from_light = aw9346_to_light;
}

void tft_fps_test(void) {
    uint8_t test_sec = 3;
    uint32_t ms = millis() + test_sec * 1000;
    uint32_t frames = 0;
    const uint32_t test_colors[] = {TFT_RED, TFT_GREEN, TFT_BLUE};
    while(ms > millis()) {
        tft_gc9a01.fillScreen(test_colors[frames % (sizeof(test_colors) / sizeof(test_colors[0]))]);
        frames++;
    }
    Serial.println("\r\n******** lcd fps test *****\r\n");
    Serial.print("fps=");
    Serial.println(frames / test_sec, DEC);
    Serial.print("test_sec=");
    Serial.println(test_sec, DEC);
    Serial.println("\r\n***************************\r\n");
}

lv_indev_t * ts_cst816s_indev;
void lvgl_hal_init(void) {
#ifdef CST816S_SUPPORT
    // touch screen
    ts_cst816s.begin();
    ts_cst816s.setReportRate(1); // 10ms, matched to LV_INDEV_DEF_READ_PERIOD
    ts_cst816s.setReportMode(0x60); // touch + gesture generated interrupt
    ts_cst816s.setMotionMask(0); // disable motion
    ts_cst816s.setAutoRst(0); // disable auto reset
    ts_cst816s.setLongRst(0); // disable long press reset
    ts_cst816s.setDisAutoSleep(1); // disable auto sleep
#endif

    // display
    tft_gc9a01.begin();
    tft_gc9a01.invertDisplay(1);
    // tft_gc9a01.setRotation(2);

    tft_gc9a01.fillScreen(TFT_BLACK);
    tft_backlight_init();
    delay(50);
    tft_set_backlight(16);

    // tft_fps_test();

    /* Draw buffers.
     *
     * This used to be a single full-screen (240x240x2 = 115KB) buffer allocated
     * with LV_MEM_CUSTOM_ALLOC, i.e. ps_malloc, so every render wrote into PSRAM
     * and every flush read all 115KB back out of it. PSRAM is reached over the
     * octal bus and is far slower than internal SRAM, and LVGL's blending does
     * scattered read-modify-write, which is its worst case.
     *
     * Two partial buffers in internal DMA-capable SRAM instead: smaller, much
     * faster to touch, and double buffering lets LVGL render the next chunk
     * while the previous one is being pushed. 40 lines is ~1/6 of the screen.
     *
     * Note LV_MEM_CUSTOM_ALLOC stays on ps_malloc (lv_conf.h) -- the object
     * tree and GIF decode buffers are fine in PSRAM, it is only the hot draw
     * path that is not. */
    #define DRAW_BUF_LINES 40
    static lv_disp_draw_buf_t draw_buf;
    const size_t draw_px = TFT_WIDTH * DRAW_BUF_LINES;
    lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(
        draw_px * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    lv_color_t *buf2 = (lv_color_t *)heap_caps_malloc(
        draw_px * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    lv_init();
    if (buf1 && buf2) {
        Serial.printf("lvgl draw buf: 2x %u px internal SRAM (%u B each)\r\n",
                      (unsigned)draw_px, (unsigned)(draw_px * sizeof(lv_color_t)));
        lv_disp_draw_buf_init(&draw_buf, buf1, buf2, draw_px);
    } else {
        // Not enough internal DRAM: fall back to the original PSRAM full-screen
        // buffer rather than failing to boot.
        if (buf1) heap_caps_free(buf1);
        if (buf2) heap_caps_free(buf2);
        Serial.println("lvgl draw buf: internal alloc failed, falling back to PSRAM");
        static lv_color_t *color_buf = (lv_color_t *)LV_MEM_CUSTOM_ALLOC(
            TFT_WIDTH * TFT_HEIGHT * sizeof(lv_color_t));
        lv_disp_draw_buf_init(&draw_buf, color_buf, NULL, TFT_WIDTH * TFT_HEIGHT);
    }

    /*Initialize the display*/
    // must static
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    /*Change the following line to your display resolution*/
    disp_drv.hor_res = TFT_WIDTH;
    disp_drv.ver_res = TFT_HEIGHT;
    disp_drv.flush_cb = usr_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);
    // lv_disp_set_rotation(NULL, LV_DISP_ROT_180);

#ifdef CST816S_SUPPORT
    /* touch screen */
    // must static
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);      /*Basic initialization*/
    indev_drv.gesture_limit = 1;
    indev_drv.gesture_min_velocity = 1;
    indev_drv.type = LV_INDEV_TYPE_POINTER;                 /*See below.*/
    indev_drv.read_cb = usr_touchpad_read;              /*See below.*/
    /*Register the driver in LVGL and save the created input device object*/
    ts_cst816s_indev = lv_indev_drv_register(&indev_drv);
#endif

    /* set background color to black (default white) */
    lv_obj_set_style_bg_color(lv_scr_act(), LV_COLOR_MAKE(0, 0, 0), LV_STATE_DEFAULT);
}
