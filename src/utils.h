#ifndef __UTILS__H__
#define __UTILS__H__

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <FS.h>
#include <LiquidCrystal_I2C.h>
#include <Audio.h>

#define SD_CS 5
#define SD_SCK 18
#define SD_MISO 19
#define SD_MOSI 23
#define LCD_SDA 21
#define LCD_SCL 22

#define I2S_LRC 25
#define I2S_BCLK 26
#define I2S_DIN 16

extern Audio audio;
extern LiquidCrystal_I2C lcd;

#endif