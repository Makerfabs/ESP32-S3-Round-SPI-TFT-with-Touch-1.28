#include <Arduino_GFX_Library.h>
#include <CST816S.h>
#include <SD.h>
#include <FS.h>
#include "JpegFunc.h"

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

#define SD_SCK 42
#define SD_MISO 41
#define SD_MOSI 2
#define SD_CS 1

#define JPEG_FILENAME "/logo_240240.jpg"

Arduino_ESP32SPI *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, TFT_MISO, HSPI, true); // Constructor
Arduino_GFX *gfx = new Arduino_GC9A01(bus, TFT_RES, 0 /* rotation */, true /* IPS */);
CST816S touch(TOUCH_SDA, TOUCH_SCL, TOUCH_RST, TOUCH_INT); // sda, scl, rst, irq

void setup(void)
{
    USBSerial.begin(115200);

    pinMode(TFT_BLK, OUTPUT);
    digitalWrite(TFT_BLK, 1);

    SPI.begin(SD_SCK, SD_MISO, SD_MOSI);

    delay(1000);
    USBSerial.println("start");

    gfx->begin();
    touch.begin();

    if (!SD.begin(SD_CS))
    {
        USBSerial.println(F("ERROR: File System Mount Failed!"));

        gfx->setTextSize(4);
        gfx->setCursor(10, 120);
        gfx->println(F("SD ERROR"));
        while (1)
            delay(3000);
    }
    else
    {
        unsigned long start = millis();
        jpegDraw(JPEG_FILENAME, jpegDrawCallback, true, 0, 0, 240, 240);
        USBSerial.printf("Time used: %lu\n", millis() - start);
        delay(2000);
    }

    gfx->fillScreen(RED);
    delay(1000);
    gfx->fillScreen(GREEN);
    delay(1000);
    gfx->fillScreen(BLUE);
    delay(1000);
    gfx->fillScreen(YELLOW);
    delay(1000);
    gfx->fillScreen(WHITE);
}

void loop()
{

    if (touch.available())
    {
        gfx->fillCircle(touch.data.x, touch.data.y, 5, RED);
    }
}

// pixel drawing callback
static int jpegDrawCallback(JPEGDRAW *pDraw)
{
    // USBSerial.printf("Draw pos = %d,%d. size = %d x %d\n", pDraw->x, pDraw->y, pDraw->iWidth, pDraw->iHeight);
    gfx->draw16bitBeRGBBitmap(pDraw->x, pDraw->y, pDraw->pPixels, pDraw->iWidth, pDraw->iHeight);
    return 1;
}
