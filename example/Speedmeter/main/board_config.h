#pragma once

#include "driver/spi_master.h"
#include "driver/i2c.h"
#include "driver/uart.h"

/* Makerfabs ESP32-S3 Round SPI TFT with Touch, 1.28-inch GC9A01 revision 1.1. */
#define BOARD_LCD_WIDTH          240
#define BOARD_LCD_HEIGHT         240
#define BOARD_LCD_MOSI           13
#define BOARD_LCD_MISO           12
#define BOARD_LCD_SCLK           14
#define BOARD_LCD_CS             15
#define BOARD_LCD_DC             21
#define BOARD_LCD_RST            11
#define BOARD_LCD_BACKLIGHT      45
#define BOARD_LCD_SPI_HOST       SPI2_HOST

#define BOARD_TOUCH_SDA          38
#define BOARD_TOUCH_SCL          39
#define BOARD_TOUCH_RST          16
#define BOARD_TOUCH_INT          40
#define BOARD_TOUCH_I2C_PORT     I2C_NUM_0
#define BOARD_TOUCH_I2C_ADDRESS  0x15

#define BOARD_SD_MOSI            2
#define BOARD_SD_MISO            41
#define BOARD_SD_SCLK            42
#define BOARD_SD_CS              1
#define BOARD_SD_SPI_HOST        SPI3_HOST
#define BOARD_SD_MOUNT_POINT     "/sdcard"

/* SR2631Z1 carrier with SR1612Z1 receiver. */
#define BOARD_GPS_UART_PORT      UART_NUM_2
#define BOARD_GPS_UART_TX_PIN    17
#define BOARD_GPS_UART_RX_PIN    18
#define BOARD_GPS_UART_BAUD      115200
#define BOARD_GPS_DEFAULT_BAUD   9600
#define BOARD_GPS_FIX_INTERVAL_MS 200
