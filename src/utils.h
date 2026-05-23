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

#define ADC1_CHANNEL 6
#define ADC_PIN 34

#define BUTTON_SELECT_PIN 27
#define BUTTON_UP 14
#define BUTTON_DOWN 15

#define LYRIC_LEAD_MS   650u   // lyric appears this many ms before it is sung


extern Audio audio;
extern LiquidCrystal_I2C lcd;

#endif