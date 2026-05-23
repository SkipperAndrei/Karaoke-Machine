#include "utils.h"


enum PlayerState {
  STATE_MENU,
  STATE_PLAYING
};

volatile PlayerState systemState = STATE_MENU; 
bool isPlaying = false; 

Audio audio;
LiquidCrystal_I2C lcd(0x27, 20, 4);

// FreeRTOS Communication Queues
QueueHandle_t playPauseQueue;
QueueHandle_t volumeQueue;
QueueHandle_t changeTrackQueue;

volatile bool resetLyricsFlag = false;

#define MAX_SONGS 30
String songList[MAX_SONGS];
int totalSongs = 0;
int highlightedIndex = 0;   
int currentTrackIndex = 0;  

#define MAX_LYRICS 250
struct LyricLine {
  uint32_t timestampMs;
  String text;
};

LyricLine currentLyrics[MAX_LYRICS];
int lyricCount = 0;

void updateMenuLCD();

void scanSDForSongs() {
  File root = SD.open("/");
  totalSongs = 0;

  if (!root) {
    Serial.println("Failed to open root directory");
    return;
  }

  File file = root.openNextFile();
  while (file && totalSongs < MAX_SONGS) {
    if (!file.isDirectory()) {
      String fileName = String(file.name());
      if (fileName.endsWith(".mp3") || fileName.endsWith(".MP3")) {
        songList[totalSongs] = fileName;
        totalSongs++;
      }
    }
    file = root.openNextFile();
  }
  root.close();
  Serial.printf("SD Scanner: Automatically loaded %d MP3 files.\n", totalSongs);
}

void parseLRC(const char* path) {
    lyricCount = 0;
    int32_t offsetMs = 0;

    File file = SD.open(path);
    if (!file) {
        Serial.println("LRC file not found!");
        for(int i=0; i<MAX_LYRICS; i++) currentLyrics[i] = {0, ""};
        return;
    }

    while (file.available() && lyricCount < MAX_LYRICS) {
        String line = file.readStringUntil('\n');
        line.trim();

        if (!line.startsWith("[")) continue;

        if (line.startsWith("[offset:")) {
            int close = line.indexOf(']');
            if (close != -1)
                offsetMs = line.substring(8, close).toInt();
            continue;
        }

        if (!isDigit(line.charAt(1))) 
          continue;

        int closeBracket = line.indexOf(']');
        if (closeBracket == -1) 
          continue;

        String timeStr = line.substring(1, closeBracket);
        int colon = timeStr.indexOf(':');
        if (colon == -1) 
          continue;

        int dot = timeStr.indexOf('.');

        uint32_t mins = timeStr.substring(0, colon).toInt();
        uint32_t secs = (dot != -1)
                        ? timeStr.substring(colon + 1, dot).toInt()
                        : timeStr.substring(colon + 1).toInt();
        uint32_t ms = 0;

        if (dot != -1) {
            String msStr = timeStr.substring(dot + 1);
            ms = msStr.toInt();
            if      (msStr.length() == 1) ms *= 100;
            else if (msStr.length() == 2) ms *= 10;
        }

        currentLyrics[lyricCount].timestampMs = (mins * 60u + secs) * 1000u + ms;
        currentLyrics[lyricCount].text = line.substring(closeBracket + 1);
        currentLyrics[lyricCount].text.trim();
        lyricCount++;
    }

    file.close();

    if (offsetMs != 0) {
        for (int i = 0; i < lyricCount; i++) {
            int32_t adjusted = (int32_t)currentLyrics[i].timestampMs + offsetMs;
            currentLyrics[i].timestampMs = (adjusted > 0) ? (uint32_t)adjusted : 0u;
        }
    }

    Serial.printf("LRC Parser: Loaded %d lines, offset %d ms.\n", lyricCount, offsetMs);
}

void updateMenuLCD() {
  lcd.clear();
  int startRow = (highlightedIndex / 4) * 4; 
  
  for (int i = 0; i < 4; i++) {
    int songIdx = startRow + i;
    if (songIdx >= totalSongs) break;

    lcd.setCursor(0, i);
    if (songIdx == highlightedIndex) {
      lcd.print(">"); 
    } else {
      lcd.print(" ");
    }
    
    lcd.print(songList[songIdx].substring(0, 19));
  }
}

void printWrappedToLCD(const char* text, int startRow) {
    static const int COLS = 20;
    static const int ROWS = 4;

    char buf[COLS + 1];
    const char* p = text;

    for (int row = 0; row < ROWS; row++) {
        if (startRow + row >= 4) break;
        lcd.setCursor(0, startRow + row);

        while (*p == ' ') p++;

        if (!*p) {
            lcd.print("                    ");
            continue;
        }

        const char* lineStart = p;
        const char* lastSpace = nullptr;
        int         len       = 0;

        while (*p && len < COLS) {
            if (*p == ' ') lastSpace = p;
            p++;
            len++;
        }

        if (*p != '\0' && *p != ' ' && lastSpace != nullptr) {
            len = (int)(lastSpace - lineStart);
            p   = lastSpace + 1;
        }

        while (len > 0 && lineStart[len - 1] == ' ') len--;

        memcpy(buf, lineStart, len);
        buf[len] = '\0';

        lcd.print(buf);
        for (int i = len; i < COLS; i++) lcd.print(' ');
    }
}

void audio_task(void *pvParameters) {
  bool shouldPlay = false;
  int targetVolume = 5;
  int nextTrackIdx = 0;

  for (;;) {
    if (xQueueReceive(changeTrackQueue, &nextTrackIdx, 0) == pdTRUE) {
      audio.stopSong();
      
      String newLrc = "/" + songList[nextTrackIdx];
      newLrc.replace(".mp3", ".lrc");
      parseLRC(newLrc.c_str());
      
      audio.connecttoFS(SD, ("/" + songList[nextTrackIdx]).c_str());
      shouldPlay = true;
    }

    if (xQueueReceive(playPauseQueue, &shouldPlay, 0) == pdTRUE) {
      audio.pauseResume();
    }

    if (shouldPlay) {
      audio.loop();
    }

    if (xQueueReceive(volumeQueue, &targetVolume, 0) == pdTRUE) {
      audio.setVolume(targetVolume);
    }

    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void ui_task(void *pvParameters) {
  pinMode(BUTTON_SELECT_PIN, INPUT_PULLUP);
  pinMode(BUTTON_NEXT_PIN, INPUT_PULLUP);
  pinMode(BUTTON_PREV_PIN, INPUT_PULLUP);
  
  bool stableSelectState = HIGH;
  bool stableNextState = HIGH;
  bool stablePrevState = HIGH;
  int lastVolume = -1;
  
  uint32_t adcThrottleCounter = 0;

  updateMenuLCD();

  for (;;) {
    bool rawSelect = digitalRead(BUTTON_SELECT_PIN);
    bool rawNext = digitalRead(BUTTON_NEXT_PIN);
    bool rawPrev = digitalRead(BUTTON_PREV_PIN);

    if (rawSelect != stableSelectState) {
      vTaskDelay(pdMS_TO_TICKS(30)); 
      if (digitalRead(BUTTON_SELECT_PIN) == rawSelect) {
        stableSelectState = rawSelect;
        if (stableSelectState == LOW) {
          if (systemState == STATE_MENU) {
            resetLyricsFlag = true; 
            systemState = STATE_PLAYING;
            isPlaying = true;
            currentTrackIndex = highlightedIndex;
            lcd.clear();
            xQueueOverwrite(changeTrackQueue, &currentTrackIndex);
          } else {
            isPlaying = !isPlaying;
            xQueueOverwrite(playPauseQueue, &isPlaying);
          }
          vTaskDelay(pdMS_TO_TICKS(200)); 
        }
      }
    }

    if (rawNext != stableNextState) {
      vTaskDelay(pdMS_TO_TICKS(30)); 
      if (digitalRead(BUTTON_NEXT_PIN) == rawNext) {
        stableNextState = rawNext;
        if (stableNextState == LOW && totalSongs > 0) {
          if (systemState == STATE_MENU) {
            highlightedIndex = (highlightedIndex + 1) % totalSongs;
            updateMenuLCD();
          }
          vTaskDelay(pdMS_TO_TICKS(150)); 
        }
      }
    }

    if (rawPrev != stablePrevState) {
      vTaskDelay(pdMS_TO_TICKS(30)); 
      if (digitalRead(BUTTON_PREV_PIN) == rawPrev) {
        stablePrevState = rawPrev;
        if (stablePrevState == LOW && totalSongs > 0) {
          if (systemState == STATE_MENU) {
            highlightedIndex = (highlightedIndex - 1 + totalSongs) % totalSongs;
            updateMenuLCD();
          }
          vTaskDelay(pdMS_TO_TICKS(150)); 
        }
      }
    }

    adcThrottleCounter++;
    if (adcThrottleCounter >= 10) {
      adcThrottleCounter = 0; 
      
      uint32_t raw_volume = analogRead(ADC_PIN); 
      float x = raw_volume / 4095.0f;
      int32_t mapped_volume = (int32_t)(x * 21.0f);
      
      if (abs(mapped_volume - lastVolume) >= 2) {
        xQueueOverwrite(volumeQueue, &mapped_volume);
        lastVolume = mapped_volume;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(25)); 
  }
}

void lyric_task(void* pvParameters) {
    int currentLine   = -1;
    int lastDrawnLine = -2;
    uint32_t   lastAudioSec  = 0;
    TickType_t lastSyncTick  = xTaskGetTickCount();

    TickType_t       xLastWakeTime = xTaskGetTickCount();
    const TickType_t xPeriod       = pdMS_TO_TICKS(50);

    for (;;) {
        vTaskDelayUntil(&xLastWakeTime, xPeriod);

        if (resetLyricsFlag) {
            currentLine = -1;
            lastDrawnLine = -2;
            lastAudioSec = 0;
            lastSyncTick = xTaskGetTickCount();
            lcd.clear();
            resetLyricsFlag = false; 
        }

        if (systemState == STATE_MENU) {
            currentLine = -1;  
            lastDrawnLine = -2;
            continue; 
        }

        const TickType_t nowTick   = xTaskGetTickCount();
        const uint32_t   audioSec  = (uint32_t)audio.getAudioCurrentTime();

        if (!isPlaying) {
            lastSyncTick = nowTick; 
        } else if (audioSec != lastAudioSec) {
            lastAudioSec = audioSec;
            lastSyncTick = nowTick;
        }

        const uint32_t subMs     = (uint32_t)((nowTick - lastSyncTick) * portTICK_PERIOD_MS);
        const uint32_t currentMs = audioSec * 1000u + subMs;

        int nextLine = -1; 

        for (int i = 0; i < lyricCount; i++) {
            if (currentMs >= currentLyrics[i].timestampMs) {
                nextLine = i;
            } else {
                break; 
            }
        }

        if (nextLine != lastDrawnLine) {
            if (nextLine == -1) {
                lcd.clear(); 
            } else {
                printWrappedToLCD(currentLyrics[nextLine].text.c_str(), 0);
            }
            lastDrawnLine = nextLine;
        }
        currentLine = nextLine;
    }
}


void audio_eof_mp3(const char *info){
  Serial.printf("Song track execution completed: %s\n", info);
  systemState = STATE_MENU;
  isPlaying = false;
  updateMenuLCD();
}

// ============================================================================
// INITIALIZATION SETUP
// ============================================================================
void setup() {
  Serial.begin(115200);
  while(!Serial);

  Wire.begin(LCD_SDA, LCD_SCL);
  lcd.init();
  lcd.backlight();
  lcd.print("System Booting..");

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS)) {
    lcd.setCursor(0, 1);
    lcd.print("SD Error!");
    while(true) vTaskDelay(100);
  }

  scanSDForSongs();

  if (totalSongs == 0) {
    lcd.clear();
    lcd.print("No MP3 files found!");
    while(true) vTaskDelay(100);
  }

  playPauseQueue = xQueueCreate(1, sizeof(bool));
  volumeQueue    = xQueueCreate(1, sizeof(int));
  changeTrackQueue = xQueueCreate(1, sizeof(int));

  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DIN);
  audio.setVolume(5);

  xTaskCreatePinnedToCore(audio_task, "Audio", 8192, NULL, 3, NULL, 1); 
  xTaskCreatePinnedToCore(ui_task,    "UI",    4096, NULL, 2, NULL, 0);       
  xTaskCreatePinnedToCore(lyric_task, "Lyrics", 4096, NULL, 1, NULL, 0); 

  vTaskDelete(NULL); 
}

void loop() {
  // Empty
}