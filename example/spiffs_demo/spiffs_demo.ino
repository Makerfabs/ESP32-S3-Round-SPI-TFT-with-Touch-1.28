#include <Arduino_GFX_Library.h>
#include "FS.h"
#include "SPIFFS.h"
#include "JpegFunc.h"

#define TFT_BLK 45
#define TFT_RES 11

#define TFT_CS 15
#define TFT_MOSI 13
#define TFT_MISO 12
#define TFT_SCLK 14
#define TFT_DC 21

Arduino_ESP32SPI *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, TFT_MISO, HSPI, true); // Constructor

Arduino_GFX *gfx = new Arduino_GC9A01(bus, TFT_RES, 0 /* rotation */, true /* IPS */);

/* You only need to format SPIFFS the first time you run a
   test or else use the SPIFFS plugin to create a partition
   https://github.com/me-no-dev/arduino-esp32fs-plugin */
#define FORMAT_SPIFFS_IF_FAILED true

void setup()
{
    pinMode(TFT_BLK, OUTPUT);
    digitalWrite(TFT_BLK, 1);

    USBSerial.begin(115200);
    if (!SPIFFS.begin(FORMAT_SPIFFS_IF_FAILED))
    {
        USBSerial.println("SPIFFS Mount Failed");
        return;
    }

    gfx->begin();

    // listDir(SPIFFS, "/", 0);
    // writeFile(SPIFFS, "/hello.txt", "Hello ");
    // appendFile(SPIFFS, "/hello.txt", "World!\r\n");
    // readFile(SPIFFS, "/hello.txt");
    // renameFile(SPIFFS, "/hello.txt", "/foo.txt");
    // readFile(SPIFFS, "/foo.txt");
    // deleteFile(SPIFFS, "/foo.txt");
    // testFileIO(SPIFFS, "/test.txt");
    // deleteFile(SPIFFS, "/test.txt");
    // USBSerial.println("Test complete");
}

int num = 0;

void loop()
{
    listDir(SPIFFS, "/", 0);

    char str[40];
    sprintf(str, "num:%d\n", num++);
    writeFile(SPIFFS, "/spiffs.txt", str);
    readFile(SPIFFS, "/spiffs.txt");
    jpegDraw("/logo_240240.jpg", jpegDrawCallback, true, 0, 0, 240, 240);
    delay(2000);
    gfx->fillScreen(YELLOW);
    delay(1000);
}

// pixel drawing callback
static int jpegDrawCallback(JPEGDRAW *pDraw)
{
    // USBSerial.printf("Draw pos = %d,%d. size = %d x %d\n", pDraw->x, pDraw->y, pDraw->iWidth, pDraw->iHeight);
    gfx->draw16bitBeRGBBitmap(pDraw->x, pDraw->y, pDraw->pPixels, pDraw->iWidth, pDraw->iHeight);
    return 1;
}

// File Function ..............................

void listDir(fs::FS &fs, const char *dirname, uint8_t levels)
{
    USBSerial.printf("Listing directory: %s\r\n", dirname);

    File root = fs.open(dirname);
    if (!root)
    {
        USBSerial.println("- failed to open directory");
        return;
    }
    if (!root.isDirectory())
    {
        USBSerial.println(" - not a directory");
        return;
    }

    File file = root.openNextFile();
    while (file)
    {
        if (file.isDirectory())
        {
            USBSerial.print("  DIR : ");
            USBSerial.println(file.name());
            if (levels)
            {
                listDir(fs, file.path(), levels - 1);
            }
        }
        else
        {
            USBSerial.print("  FILE: ");
            USBSerial.print(file.name());
            USBSerial.print("\tSIZE: ");
            USBSerial.println(file.size());
        }
        file = root.openNextFile();
    }
}

void readFile(fs::FS &fs, const char *path)
{
    USBSerial.printf("Reading file: %s\r\n", path);

    File file = fs.open(path);
    if (!file || file.isDirectory())
    {
        USBSerial.println("- failed to open file for reading");
        return;
    }

    USBSerial.println("- read from file:");
    while (file.available())
    {
        USBSerial.write(file.read());
    }
    file.close();
}

void writeFile(fs::FS &fs, const char *path, const char *message)
{
    USBSerial.printf("Writing file: %s\r\n", path);

    File file = fs.open(path, FILE_WRITE);
    if (!file)
    {
        USBSerial.println("- failed to open file for writing");
        return;
    }
    if (file.print(message))
    {
        USBSerial.println("- file written");
    }
    else
    {
        USBSerial.println("- write failed");
    }
    file.close();
}

void appendFile(fs::FS &fs, const char *path, const char *message)
{
    USBSerial.printf("Appending to file: %s\r\n", path);

    File file = fs.open(path, FILE_APPEND);
    if (!file)
    {
        USBSerial.println("- failed to open file for appending");
        return;
    }
    if (file.print(message))
    {
        USBSerial.println("- message appended");
    }
    else
    {
        USBSerial.println("- append failed");
    }
    file.close();
}

void renameFile(fs::FS &fs, const char *path1, const char *path2)
{
    USBSerial.printf("Renaming file %s to %s\r\n", path1, path2);
    if (fs.rename(path1, path2))
    {
        USBSerial.println("- file renamed");
    }
    else
    {
        USBSerial.println("- rename failed");
    }
}

void deleteFile(fs::FS &fs, const char *path)
{
    USBSerial.printf("Deleting file: %s\r\n", path);
    if (fs.remove(path))
    {
        USBSerial.println("- file deleted");
    }
    else
    {
        USBSerial.println("- delete failed");
    }
}

void testFileIO(fs::FS &fs, const char *path)
{
    USBSerial.printf("Testing file I/O with %s\r\n", path);

    static uint8_t buf[512];
    size_t len = 0;
    File file = fs.open(path, FILE_WRITE);
    if (!file)
    {
        USBSerial.println("- failed to open file for writing");
        return;
    }

    size_t i;
    USBSerial.print("- writing");
    uint32_t start = millis();
    for (i = 0; i < 2048; i++)
    {
        if ((i & 0x001F) == 0x001F)
        {
            USBSerial.print(".");
        }
        file.write(buf, 512);
    }
    USBSerial.println("");
    uint32_t end = millis() - start;
    USBSerial.printf(" - %u bytes written in %u ms\r\n", 2048 * 512, end);
    file.close();

    file = fs.open(path);
    start = millis();
    end = start;
    i = 0;
    if (file && !file.isDirectory())
    {
        len = file.size();
        size_t flen = len;
        start = millis();
        USBSerial.print("- reading");
        while (len)
        {
            size_t toRead = len;
            if (toRead > 512)
            {
                toRead = 512;
            }
            file.read(buf, toRead);
            if ((i++ & 0x001F) == 0x001F)
            {
                USBSerial.print(".");
            }
            len -= toRead;
        }
        USBSerial.println("");
        end = millis() - start;
        USBSerial.printf("- %u bytes read in %u ms\r\n", flen, end);
        file.close();
    }
    else
    {
        USBSerial.println("- failed to open file for reading");
    }
}
