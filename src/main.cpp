#include "utils.h"

QueueHandle_t playPauseQueue;
QueueHandle_t volumeQueue;



Audio audio;
LiquidCrystal_I2C lcd(0x27, 20, 4);

bool isPlaying = true; 

#define MAX_LYRICS 250
struct LyricLine {
  uint32_t timestampMs;
  String text;
};

LyricLine currentLyrics[MAX_LYRICS];
int lyricCount = 0;

void parseLRC(const char* path)
{
    lyricCount = 0;
    int32_t offsetMs = 0;

    File file = SD.open(path);
    if (!file) {
        Serial.println("LRC file not found!");
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


void audio_task(void *pvParameters) {
  bool shouldPlay = true;
  int targetVolume = 35;

  for (;;) {
    if (shouldPlay) {
      audio.loop();
    }

    if (xQueueReceive(playPauseQueue, &shouldPlay, 0) == pdTRUE) {
      audio.pauseResume();
    }

    if (xQueueReceive(volumeQueue, &targetVolume, 0) == pdTRUE) {
      audio.setVolume(targetVolume);
    }

    vTaskDelay(pdMS_TO_TICKS(2));
  }
}



void ui_task(void *pvParameters) {
  pinMode(BUTTON_SELECT_PIN, INPUT_PULLUP);
  
  bool stableButtonState = HIGH;
  int lastVolume = -1;

  for (;;) {
    bool rawButtonState = digitalRead(BUTTON_SELECT_PIN);
    
    if (rawButtonState != stableButtonState) {
      vTaskDelay(pdMS_TO_TICKS(50)); 
      
      if (digitalRead(BUTTON_SELECT_PIN) == rawButtonState) {
        stableButtonState = rawButtonState;
        
        if (stableButtonState == LOW) {
          isPlaying = !isPlaying;
          xQueueOverwrite(playPauseQueue, &isPlaying);
        }
      }
    }

    uint32_t raw_volume = analogRead(ADC_PIN); 
    float x = raw_volume / 4095.0f;
    int32_t mapped_volume = (int32_t)(x * 21.0f);
    
    if (abs(mapped_volume - lastVolume) >= 2) {
      xQueueOverwrite(volumeQueue, &mapped_volume);
      lastVolume = mapped_volume;
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void printWrappedToLCD(const char* text, int startRow)
{
    static const int COLS = 20;
    static const int ROWS = 4;

    char        buf[COLS + 1];
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

void lyric_task(void* pvParameters)
{
    int currentLine   = -1;
    int lastDrawnLine = -2;

    uint32_t   lastAudioSec  = 0;
    TickType_t lastSyncTick  = xTaskGetTickCount();

    TickType_t       xLastWakeTime = xTaskGetTickCount();
    const TickType_t xPeriod       = pdMS_TO_TICKS(50);

    for (;;) {
        vTaskDelayUntil(&xLastWakeTime, xPeriod);

        const TickType_t nowTick   = xTaskGetTickCount();
        const uint32_t   audioSec  = (uint32_t)audio.getAudioCurrentTime();

        if (audioSec != lastAudioSec) {
            lastAudioSec = audioSec;
            lastSyncTick = nowTick;
        }

        const uint32_t subMs     = (uint32_t)((nowTick - lastSyncTick) * portTICK_PERIOD_MS);
        const uint32_t currentMs = audioSec * 1000u + subMs;

        int nextLine = currentLine;

        while (nextLine < (int)lyricCount - 1 &&
               currentMs + LYRIC_LEAD_MS >= currentLyrics[nextLine + 1].timestampMs)
        {
            nextLine++;
        }

        if (nextLine != lastDrawnLine && nextLine >= 0) {
            printWrappedToLCD(currentLyrics[nextLine].text.c_str(), 0);
            lastDrawnLine = nextLine;
        }

        currentLine = nextLine;
    }
}

void setup() {
  Serial.begin(115200);
  while(!Serial);

  Wire.begin(LCD_SDA, LCD_SCL);
  lcd.init();
  lcd.setCursor(0, 0);
  lcd.backlight();
  lcd.print("System Booting..");

  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS)) {
    lcd.setCursor(0, 1);
    lcd.print("SD Error!");
    while(true) vTaskDelay(100);
  }

  const char* lrcFile = "/detroit_rock_city.lrc";
  const char* mp3File = "/detroit.mp3";

  lcd.clear();
  lcd.print("Loading Lyrics..");
  parseLRC(lrcFile);

  // Initialize Audio
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DIN);
  audio.setVolume(2);
  audio.connecttoFS(SD, mp3File); 

  playPauseQueue = xQueueCreate(1, sizeof(bool));
  volumeQueue = xQueueCreate(1, sizeof(int));

  // Launch tasks
  xTaskCreatePinnedToCore(audio_task, "Audio", 8192, NULL, 3, NULL, 0); 
  xTaskCreatePinnedToCore(ui_task, "UI", 4096, NULL, 2, NULL, 1);       
  xTaskCreatePinnedToCore(lyric_task, "Lyrics", 4096, NULL, 1, NULL, 1); 

  vTaskDelete(NULL); 
}

void loop() { }