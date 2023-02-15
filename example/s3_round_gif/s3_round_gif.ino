

#include <Arduino_GFX_Library.h>
#include <SD.h>
#include <CST816S.h>

#include "GifClass.h"

#define TFT_BLK 45
#define TFT_RES 11
#define TFT_DC 21

#define TFT_CS 15
#define TFT_MOSI 13
#define TFT_MISO 12
#define TFT_SCLK 14

#define SD_SCK 42
#define SD_MISO 41
#define SD_MOSI 2
#define SD_CS 1

#define TOUCH_INT 40
#define TOUCH_SDA 38
#define TOUCH_SCL 39
#define TOUCH_RST 16

#define GIF_NUM 5
String gif_list[GIF_NUM] =
    {
        "/test1.gif",
        "/test2.gif",
        "/test3.gif",
        "/test4.gif",
        "/test5.gif"};
int gif_bg[GIF_NUM] =
    {
        WHITE,
        WHITE,
        BLACK,
        BLACK,
        BLACK};

int gif_index = 0;
int fresh_flag = 1;

Arduino_ESP32SPI *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, TFT_MISO, HSPI, true); // Constructor
Arduino_GFX *gfx = new Arduino_GC9A01(bus, TFT_RES, 0 /* rotation */, true /* IPS */);
CST816S touch(TOUCH_SDA, TOUCH_SCL, TOUCH_RST, TOUCH_INT);

static GifClass gifClass;

void setup()
{
    pinMode(TFT_BLK, OUTPUT);
    digitalWrite(TFT_BLK, HIGH);

    USBSerial.begin(115200);
    USBSerial.println("Animated GIF Image Viewer");

    // Init Display
    gfx->begin();

    SPI.begin(SD_SCK, SD_MISO, SD_MOSI);
    if (!SD.begin(SD_CS))
    {
        USBSerial.println(F("ERROR: File System Mount Failed!"));
        gfx->println(F("ERROR: File System Mount Failed!"));
    }

    touch.begin();

    xTaskCreatePinnedToCore(Task_TFT, "Task_TFT", 20480, NULL, 0, NULL, 0);
    xTaskCreatePinnedToCore(Task_touch, "Task_touch", 10240, NULL, 3, NULL, 1);
}

void loop()
{
}

void Task_TFT(void *pvParameters)
{
    while (1)
    {
        if (fresh_flag == 1)
        {
            gfx->fillScreen(gif_bg[gif_index]);
            fresh_flag = 0;
        }

        File gifFile = SD.open(gif_list[gif_index], "r");
        if (!gifFile || gifFile.isDirectory())
        {
            USBSerial.println(F("ERROR: open gifFile Failed!"));
            gfx->println(F("ERROR: open gifFile Failed!"));
        }
        else
        {
            // read GIF file header
            gd_GIF *gif = gifClass.gd_open_gif(&gifFile);
            if (!gif)
            {
                USBSerial.println(F("gd_open_gif() failed!"));
            }
            else
            {
                uint8_t *buf = (uint8_t *)malloc(gif->width * gif->height);
                if (!buf)
                {
                    USBSerial.println(F("buf malloc failed!"));
                }
                else
                {
                    int16_t x = 120 - gif->width / 2;
                    int16_t y = 120 - gif->height / 2;

                    USBSerial.println(F("GIF video start"));
                    int32_t t_fstart, t_delay = 0, t_real_delay, delay_until;
                    int32_t res = 1;
                    int32_t duration = 0, remain = 0;
                    while (res > 0)
                    {
                        t_fstart = millis();
                        t_delay = gif->gce.delay * 10;
                        res = gifClass.gd_get_frame(gif, buf);
                        if (res < 0)
                        {
                            USBSerial.println(F("ERROR: gd_get_frame() failed!"));
                            break;
                        }
                        else if (res > 0)
                        {
                            gfx->drawIndexedBitmap(x, y, buf, gif->palette->colors, gif->width, gif->height);

                            t_real_delay = t_delay - (millis() - t_fstart);
                            duration += t_delay;
                            remain += t_real_delay;
                            delay_until = millis() + t_real_delay;
                            while (millis() < delay_until)
                            {
                                vTaskDelay(1);
                            }

                            if (fresh_flag == 1)
                            {
                                break;
                            }
                        }
                    }
                    // USBSerial.println(F("GIF video end"));
                    // USBSerial.print(F("duration: "));
                    // USBSerial.print(duration);
                    // USBSerial.print(F(", remain: "));
                    // USBSerial.print(remain);
                    // USBSerial.print(F(" ("));
                    // USBSerial.print(100.0 * remain / duration);
                    // USBSerial.println(F("%)"));

                    gifClass.gd_close_gif(gif);
                    free(buf);
                }
            }
        }
    }
}

void Task_touch(void *pvParameters)
{

    long runtime_1 = 0;

    while (1)
    {
        if (touch.available())
        {

            if ((millis() - runtime_1) > 1000)
            {
                gif_index++;
                gif_index %= GIF_NUM;
                fresh_flag = 1;

                runtime_1 = millis();
                // USBSerial.println(F("Get touch"));
            }
        }
        vTaskDelay(100);
    }
}
