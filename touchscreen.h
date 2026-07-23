#pragma once
/* Touch handling. Capacitive is the default JC4827W543C variant; set
 * TOUCH_CAPACITIVE to 0 in the .ino for the resistive board.                */

TOUCHINFO ti;
uint16_t  touchMinX = 1, touchMaxX = 480;
uint16_t  touchMinY = 1, touchMaxY = 272;

/* last raw + mapped sample, for the touch diagnostics screen */
volatile int16_t tt_raw_x = 0, tt_raw_y = 0;
volatile int16_t tt_map_x = 0, tt_map_y = 0;
volatile bool    tt_pressed = false;
volatile uint32_t tt_events = 0;

#if TOUCH_CAPACITIVE
BBCapTouch bbct;
#else
BB_SPI_LCD *lcd_ptr = nullptr;
#endif

void touch_read(lv_indev_t *indev, lv_indev_data_t *data) {
#if TOUCH_CAPACITIVE
    if (bbct.getSamples(&ti)) {
        if (ti.x[0] < touchMinX) touchMinX = ti.x[0];
        if (ti.x[0] > touchMaxX) touchMaxX = ti.x[0];
        if (ti.y[0] < touchMinY) touchMinY = ti.y[0];
        if (ti.y[0] > touchMaxY) touchMaxY = ti.y[0];
        data->point.x = lv_display_get_horizontal_resolution(NULL) - map(ti.x[0], touchMinX, touchMaxX, 1,
                            lv_display_get_horizontal_resolution(NULL));
        data->point.y = lv_display_get_vertical_resolution(NULL) - map(ti.y[0], touchMinY, touchMaxY, 1,
                            lv_display_get_vertical_resolution(NULL));
        data->state   = LV_INDEV_STATE_PRESSED;
        tt_raw_x = ti.x[0]; tt_raw_y = ti.y[0];
        tt_map_x = data->point.x; tt_map_y = data->point.y;
        if (!tt_pressed) tt_events++;
        tt_pressed = true;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        tt_pressed  = false;
    }
#else
    if (lcd_ptr && lcd_ptr->rtReadTouch(&ti)) {
        data->point.x = ti.x[0];
        data->point.y = ti.y[0];
        data->state   = LV_INDEV_STATE_PRESSED;
        tt_raw_x = ti.x[0]; tt_raw_y = ti.y[0];
        tt_map_x = ti.x[0]; tt_map_y = ti.y[0];
        if (!tt_pressed) tt_events++;
        tt_pressed = true;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        tt_pressed  = false;
    }
#endif
}

void touch_reset_calibration() {
    touchMinX = 1; touchMaxX = 480;
    touchMinY = 1; touchMaxY = 272;
    tt_events = 0;
}

void touch_setup() {
#if TOUCH_CAPACITIVE
    bbct.init(TOUCH_SDA, TOUCH_SCL, TOUCH_RST, TOUCH_INT);
#else
    lv_bb_spi_lcd_t *dsc = (lv_bb_spi_lcd_t *)lv_display_get_driver_data(disp);
    lcd_ptr = dsc->lcd;
    lcd_ptr->rtInit(TOUCH_MOSI, TOUCH_MISO, TOUCH_CLK, TOUCH_CS);
#endif
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read);
}
