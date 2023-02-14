

#include <Arduino_GFX_Library.h>
#include <SD.h>

#include "GifClass.h"

#define GIF_FILENAME "/test1.gif"

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

Arduino_ESP32SPI *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, TFT_MISO, HSPI, true); // Constructor
Arduino_GFX *gfx = new Arduino_GC9A01(bus, TFT_RES, 0 /* rotation */, true /* IPS */);

static GifClass gifClass;

void setup()
{
    pinMode(TFT_BLK, OUTPUT);
    digitalWrite(TFT_BLK, HIGH);

    Serial.begin(115200);
    Serial.println("Animated GIF Image Viewer");

    // Init Display
    gfx->begin();
    gfx->fillScreen(BLACK);

    SPI.begin(SD_SCK, SD_MISO, SD_MOSI);
    if (!SD.begin(SD_CS))
    {
        Serial.println(F("ERROR: File System Mount Failed!"));
        gfx->println(F("ERROR: File System Mount Failed!"));
    }

    xTaskCreatePinnedToCore(Task_TFT, "Task_TFT", 20480, NULL, 0, NULL, 0);
}

void loop()
{
}

void Task_TFT(void *pvParameters)
{
    while (1)
    {
        File gifFile = SD.open(GIF_FILENAME, "r");
        if (!gifFile || gifFile.isDirectory())
        {
            Serial.println(F("ERROR: open gifFile Failed!"));
            gfx->println(F("ERROR: open gifFile Failed!"));
        }
        else
        {
            // read GIF file header
            gd_GIF *gif = gifClass.gd_open_gif(&gifFile);
            if (!gif)
            {
                Serial.println(F("gd_open_gif() failed!"));
            }
            else
            {
                uint8_t *buf = (uint8_t *)malloc(gif->width * gif->height);
                if (!buf)
                {
                    Serial.println(F("buf malloc failed!"));
                }
                else
                {
                    int16_t x = 120 - gif->width / 2;
                    int16_t y = 120 - gif->height / 2;

                    Serial.println(F("GIF video start"));
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
                            Serial.println(F("ERROR: gd_get_frame() failed!"));
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
                        }
                    }
                    Serial.println(F("GIF video end"));
                    Serial.print(F("duration: "));
                    Serial.print(duration);
                    Serial.print(F(", remain: "));
                    Serial.print(remain);
                    Serial.print(F(" ("));
                    Serial.print(100.0 * remain / duration);
                    Serial.println(F("%)"));

                    gifClass.gd_close_gif(gif);
                    free(buf);
                }
            }
        }
    }
}
