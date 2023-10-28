#include <lvgl.h>
#include <TFT_eSPI.h>
#include <ui.h>
#include <CST816S.h>

#define TFT_BLK 45
#define TFT_RES 11

#define TFT_CS 15
#define TFT_MOSI 13
#define TFT_MISO 12
#define TFT_SCLK 14
#define TFT_DC 21

#define TOUCH_INT 40
#define TOUCH_SDA 38
#define TOUCH_SCL 39
#define TOUCH_RST 16

#define ADC_INPUT_1 17

/*Don't forget to set Sketchbook location in File/Preferencesto the path of your UI project (the parent foder of this INO file)*/

/*Change to your screen resolution*/
static const uint16_t screenWidth = 240;
static const uint16_t screenHeight = 240;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[screenWidth * screenHeight / 10];

TFT_eSPI tft = TFT_eSPI(screenWidth, screenHeight);        /* TFT instance */
CST816S touch(TOUCH_SDA, TOUCH_SCL, TOUCH_RST, TOUCH_INT); // sda, scl, rst, irq

#if LV_USE_LOG != 0
/* USBSerial debugging */
void my_print(const char *buf)
{
    USBSerial.printf(buf);
    USBSerial.flush();
}
#endif

/* Display flushing */
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t *)&color_p->full, w * h, true);
    tft.endWrite();

    lv_disp_flush_ready(disp);
}

/*Read the touchpad*/
void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data)
{
    uint16_t touchX = 0, touchY = 0;

    bool touched = false; // tft.getTouch( &touchX, &touchY, 600 );

    if (!touch.available())
    {
        data->state = LV_INDEV_STATE_REL;
    }
    else
    {
        data->state = LV_INDEV_STATE_PR;

        /*Set the coordinates*/
        touchX = touch.data.x;
        touchY = touch.data.y;

        data->point.x = touchX;
        data->point.y = touchY;

        USBSerial.print("Data x ");
        USBSerial.println(touchX);

        USBSerial.print("Data y ");
        USBSerial.println(touchY);
    }
}

void setup()
{
    pinMode(TFT_BLK, OUTPUT);
    digitalWrite(TFT_BLK, 1);

    pinMode(ADC_INPUT_1, INPUT);

    USBSerial.begin(115200); /* prepare for possible serial debug */

    String LVGL_Arduino = "Hello Arduino! ";
    LVGL_Arduino += String('V') + lv_version_major() + "." + lv_version_minor() + "." + lv_version_patch();

    USBSerial.println(LVGL_Arduino);
    USBSerial.println("I am LVGL_Arduino");

    touch.begin();

    lv_init();

#if LV_USE_LOG != 0
    lv_log_register_print_cb(my_print); /* register print function for debugging */
#endif

    tft.begin();        /* TFT init */
    tft.setRotation(0); /* Landscape orientation, flipped */

    lv_disp_draw_buf_init(&draw_buf, buf, NULL, screenWidth * screenHeight / 10);

    /*Initialize the display*/
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    /*Change the following line to your display resolution*/
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    /*Initialize the (dummy) input device driver*/
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    ui_init();

    USBSerial.println("Setup done");
}

long runtime = 0;

void loop()
{
    if ((millis() - runtime) > 100)
    {
        int adc = analogRead(ADC_INPUT_1);
        long int adc_percent = adc * 100 / 4096;
        lv_arc_set_value(ui_Arc1, (int)adc_percent);
        lv_arc_set_value(ui_Arc2, (int)adc_percent);
        lv_arc_set_value(ui_Arc3, (int)adc_percent);

        // String lab_str = "";
        // lab_str = lab_str + adc_percent;

        // lv_label_set_text(ui_Label1, lab_str.c_str());
        // runtime = millis();
    }
    lv_timer_handler(); /* let the GUI do its work */
    delay(5);
}
