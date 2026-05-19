#include "utils.h"
#include "tasks.h"


void setup() {
  Serial.begin(115200);
  while(!Serial);
  Serial.print("Here we go again!\n");

  analogReadResolution(12);
  analogSetPinAttenuation(ADC_PIN, ADC_11db);

  Wire.begin(LCD_SDA, LCD_SCL);

  lcd.init();
  Serial.println("Am initializat LCD-ul");

  lcd.setCursor(0, 0);
  lcd.print("Initializing...");
  
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  if (!SD.begin(SD_CS)) {
    lcd.print("SD card mount");
    lcd.setCursor(0, 1);
    lcd.print("failed!");
    return;
  }
  lcd.setCursor(0, 1);
  lcd.print("SD card mounted");
  lcd.setCursor(0, 2);
  lcd.print("successfully!");

  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DIN);
  // audio.setVolume(35);

  audio.setVolume(1);
  
  audio.connecttoFS(SD, "/welcome_jungle.mp3");

  // Taskul pentru meniu
  // xTaskCreatePinnedToCore(menu_task, "Menu task", 4096, NULL, 1, NULL, 0);

  // Taskul pentru versuri
  // xTaskCreatePinnedToCore(lyric_task, "Lyric task", 4096, NULL, 1, NULL, 0);

  // Taskul pentru audio
  // xTaskCreatePinnedToCore(audio_task, "Audio task", 4096, NULL, 3, NULL, 1);
}

uint32_t raw;
float x, volume;
uint32_t audio_volume;

void loop() {
  // audio.loop();

  // if (audio.getAudioCurrentTime() >= 50) {
  //   audio.stopSong();
  //   Serial.println("Stop!");
  // }

  // Testing ADC
  raw = analogRead(ADC_PIN);
  x = raw * (1.0f / 4095.0f);
  audio_volume = x * 21.0f;

  Serial.println("Raw value: " + String(raw) + ", Volume: " + String(audio_volume));
  audio.setVolume(audio_volume);
  audio.loop();
}


