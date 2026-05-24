#include "utils.h"
#include <string.h>
#include <ctype.h>

enum PlayerState {
  STATE_MENU,
  STATE_PLAYING
};

volatile PlayerState system_state = STATE_MENU; 
volatile bool is_playing = false; 

Audio audio;
LiquidCrystal_I2C lcd(0x27, 20, 4);
const int COLS = 20;
const int ROWS = 4;

QueueHandle_t play_pause_queue;
QueueHandle_t volume_queue;
QueueHandle_t change_track_queue;

volatile bool reset_lyrics_flag = false;

#define MAX_SONGS 30
#define MAX_FILENAME_LEN 64
char song_list[MAX_SONGS][MAX_FILENAME_LEN];
int total_songs = 0;
int highlighted_index = 0;   
int current_track_index = 0;  

#define MAX_LYRICS 250
#define MAX_LYRIC_TEXT_LEN 80
struct LyricLine {
  uint32_t timestamp_ms;
  char text[MAX_LYRIC_TEXT_LEN];
};

LyricLine current_lyrics[MAX_LYRICS];
int lyric_count = 0;

void update_menu_lcd();

bool has_mp3_extension(const char* filename) {
  size_t len = strlen(filename);
  if (len < 4) return false;
  const char* ext = filename + len - 4;
  return (strcasecmp(ext, ".mp3") == 0);
}

void scan_sd_for_songs() {
  File root = SD.open("/");
  total_songs = 0;

  if (!root) {
    Serial.println("Failed to open root directory");
    return;
  }

  File file = root.openNextFile();
  while (file && total_songs < MAX_SONGS) {
    if (!file.isDirectory()) {
      const char* file_name = file.name();
      if (has_mp3_extension(file_name)) {
        strncpy(song_list[total_songs], file_name, MAX_FILENAME_LEN - 1);
        song_list[total_songs][MAX_FILENAME_LEN - 1] = '\0';
        total_songs++;
      }
    }
    file = root.openNextFile();
  }
  root.close();
  Serial.printf("SD Scanner: Automatically loaded %d MP3 files.\n", total_songs);
}

bool read_line_from_file(File &file, char* buffer, size_t max_len) {
  if (!file.available()) return false;
  
  size_t index = 0;
  while (file.available() && index < (max_len - 1)) {
    int c = file.read();
    if (c < 0) 
      break;
    if (c == '\r') 
      continue;
    if (c == '\n') 
      break;
    buffer[index++] = (char)c;
  }
  buffer[index] = '\0';
  return true;
}

void trim_c_str(char* str) {
  size_t len = strlen(str);
  while (len > 0 && isspace((unsigned char)str[len - 1])) {
    str[--len] = '\0';
  }
  size_t start = 0;
  while (str[start] && isspace((unsigned char)str[start])) {
    start++;
  }
  if (start > 0) {
    memmove(str, str + start, len - start + 1);
  }
}

void parse_lrc(const char* path) {
    lyric_count = 0;
    int32_t offset_ms = 0;
    char line[128];

    File file = SD.open(path);
    if (!file) {
        Serial.println("LRC file not found!");
        for(int i = 0; i < MAX_LYRICS; i++) {
          current_lyrics[i].timestamp_ms = 0;
          current_lyrics[i].text[0] = '\0';
        }
        return;
    }

    while (file.available() && lyric_count < MAX_LYRICS) {
        if (!read_line_from_file(file, line, sizeof(line))) continue;
        trim_c_str(line);

        if (line[0] != '[') 
          continue;

        if (strncmp(line, "[offset:", 8) == 0) {
            char* close = strchr(line, ']');
            if (close != NULL) {
                offset_ms = atoi(line + 8);
            }
            continue;
        }

        if (!isdigit((unsigned char)line[1])) 
          continue;

        char* close_bracket = strchr(line, ']');
        if (close_bracket == NULL) 
          continue;

        char* colon = strchr(line + 1, ':');
        if (colon == NULL || colon > close_bracket) 
          continue;

        char* dot = strchr(line + 1, '.');
        if (dot > close_bracket) dot = NULL;

        uint32_t mins = atoi(line + 1);
        uint32_t secs = atoi(colon + 1);
        uint32_t ms = 0;

        if (dot != NULL) {
            ms = atoi(dot + 1);
            size_t ms_digit_len = close_bracket - (dot + 1);
            if (ms_digit_len == 1) 
              ms *= 100;
            else if (ms_digit_len == 2) 
              ms *= 10;
        }

        current_lyrics[lyric_count].timestamp_ms = (mins * 60u + secs) * 1000u + ms;
        
        strncpy(current_lyrics[lyric_count].text, close_bracket + 1, MAX_LYRIC_TEXT_LEN - 1);
        current_lyrics[lyric_count].text[MAX_LYRIC_TEXT_LEN - 1] = '\0';
        trim_c_str(current_lyrics[lyric_count].text);
        
        lyric_count++;
    }

    file.close();

    if (offset_ms != 0) {
        for (int i = 0; i < lyric_count; i++) {
            int32_t adjusted = (int32_t)current_lyrics[i].timestamp_ms + offset_ms;
            current_lyrics[i].timestamp_ms = (adjusted > 0) ? (uint32_t)adjusted : 0u;
        }
    }

    Serial.printf("LRC Parser: Loaded %d lines, offset %d ms.\n", lyric_count, offset_ms);
}

void update_menu_lcd() {
  lcd.clear();
  int start_row = (highlighted_index / 4) * 4; 
  
  for (int i = 0; i < 4; i++) {
    int song_idx = start_row + i;
    if (song_idx >= total_songs) 
      break;

    lcd.setCursor(0, i);
    if (song_idx == highlighted_index) {
      lcd.print(">"); 
    } else {
      lcd.print(" ");
    }
    
    char temp_disp[20];
    strncpy(temp_disp, song_list[song_idx], 19);
    temp_disp[19] = '\0';
    lcd.print(temp_disp);
  }
}

void print_wrapped_to_lcd(const char* text, int start_row) {
    char buf[COLS + 1];
    const char* p = text;
    int len = 0;

    for (int row = 0; row < ROWS; row++) {
        if (start_row + row >= 4) 
          break;
        lcd.setCursor(0, start_row + row);

        while (*p == ' ') 
          p++;

        if (!*p) {
            lcd.print("                    ");
            continue;
        }

        const char* line_start = p;
        const char* last_space = nullptr;
        len = 0;

        while (*p && len < COLS) {
            if (*p == ' ') 
              last_space = p;
            p++;
            len++;
        }

        if (*p != '\0' && *p != ' ' && last_space != nullptr) {
            len = (int)(last_space - line_start);
            p = last_space + 1;
        }

        while (len > 0 && line_start[len - 1] == ' ') 
          len--;

        memcpy(buf, line_start, len);
        buf[len] = '\0';

        lcd.print(buf);
        for (int i = len; i < COLS; i++) 
          lcd.print(' ');
    }
}

void audio_task(void *pvParameters) {
  bool should_play = false;
  int target_volume = 5;
  int next_track_idx = 0;
  char path_buf[MAX_FILENAME_LEN + 2];

  for (;;) {
    if (xQueueReceive(change_track_queue, &next_track_idx, 0) == pdTRUE) {
      audio.stopSong();
      
      snprintf(path_buf, sizeof(path_buf), "/%s", song_list[next_track_idx]);
      
      char* ext = strrchr(path_buf, '.');
      if (ext != NULL && (strcasecmp(ext, ".mp3") == 0)) {
         strcpy(ext, ".lrc");
      }
      
      parse_lrc(path_buf);
      
      snprintf(path_buf, sizeof(path_buf), "/%s", song_list[next_track_idx]);
      audio.connecttoFS(SD, path_buf);
      should_play = true;
    }

    if (xQueueReceive(play_pause_queue, &should_play, 0) == pdTRUE) {
      audio.pauseResume();
    }

    if (should_play) {
      audio.loop();
    }

    if (xQueueReceive(volume_queue, &target_volume, 0) == pdTRUE) {
      audio.setVolume(target_volume);
    }

    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void ui_task(void *pvParameters) {
  pinMode(BUTTON_SELECT_PIN, INPUT_PULLUP);
  pinMode(BUTTON_NEXT_PIN, INPUT_PULLUP);
  pinMode(BUTTON_PREV_PIN, INPUT_PULLUP);
  
  bool stable_select_state = HIGH;
  bool stable_next_state = HIGH;
  bool stable_prev_state = HIGH;
  bool raw_select = HIGH;
  bool raw_next = HIGH;
  bool raw_prev = HIGH;
  int last_volume = -1;
  
  uint32_t adc_throttle_counter = 0;

  update_menu_lcd();

  for (;;) {
    raw_select = digitalRead(BUTTON_SELECT_PIN);
    raw_next = digitalRead(BUTTON_NEXT_PIN);
    raw_prev = digitalRead(BUTTON_PREV_PIN);

    if (raw_select != stable_select_state) {
      vTaskDelay(pdMS_TO_TICKS(30)); 
      if (digitalRead(BUTTON_SELECT_PIN) == raw_select) {
        stable_select_state = raw_select;
        if (stable_select_state == LOW) {
          if (system_state == STATE_MENU) {
            reset_lyrics_flag = true; 
            system_state = STATE_PLAYING;
            is_playing = true;
            current_track_index = highlighted_index;
            lcd.clear();
            xQueueOverwrite(change_track_queue, &current_track_index);
          } else {
            is_playing = !is_playing;
            xQueueOverwrite(play_pause_queue, const_cast<bool*>(&is_playing));
          }
          vTaskDelay(pdMS_TO_TICKS(200)); 
        }
      }
    }

    if (raw_next != stable_next_state) {
      vTaskDelay(pdMS_TO_TICKS(30)); 
      if (digitalRead(BUTTON_NEXT_PIN) == raw_next) {
        stable_next_state = raw_next;
        if (stable_next_state == LOW && total_songs > 0) {
          if (system_state == STATE_MENU) {
            highlighted_index = (highlighted_index + 1) % total_songs;
            update_menu_lcd();
          }
          vTaskDelay(pdMS_TO_TICKS(150)); 
        }
      }
    }

    if (raw_prev != stable_prev_state) {
      vTaskDelay(pdMS_TO_TICKS(30)); 
      if (digitalRead(BUTTON_PREV_PIN) == raw_prev) {
        stable_prev_state = raw_prev;
        if (stable_prev_state == LOW && total_songs > 0) {
          if (system_state == STATE_MENU) {
            highlighted_index = (highlighted_index - 1 + total_songs) % total_songs;
            update_menu_lcd();
          }
          vTaskDelay(pdMS_TO_TICKS(150)); 
        }
      }
    }

    adc_throttle_counter++;
    if (adc_throttle_counter >= 10) {
      adc_throttle_counter = 0; 
      
      uint32_t raw_volume = analogRead(ADC_PIN); 
      float x = raw_volume / 4095.0f;
      int32_t mapped_volume = (int32_t)(x * 21.0f);
      
      if (abs(mapped_volume - last_volume) >= 2) {
        xQueueOverwrite(volume_queue, &mapped_volume);
        last_volume = mapped_volume;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(5)); 
  }
}

void lyric_task(void* pvParameters) {
    int current_line   = -1;
    int last_drawn_line = -2;
    uint32_t last_audio_sec  = 0;
    TickType_t last_sync_tick  = xTaskGetTickCount();

    TickType_t x_last_wake_time = xTaskGetTickCount();
    const TickType_t x_period = pdMS_TO_TICKS(50);

    int next_line = -1;
    for (;;) {
        vTaskDelayUntil(&x_last_wake_time, x_period);

        if (reset_lyrics_flag) {
            current_line = -1;
            last_drawn_line = -2;
            last_audio_sec = 0;
            last_sync_tick = xTaskGetTickCount();
            lcd.clear();
            reset_lyrics_flag = false; 
        }

        if (system_state == STATE_MENU) {
            current_line = -1;  
            last_drawn_line = -2;
            continue; 
        }

        const TickType_t now_tick   = xTaskGetTickCount();
        const uint32_t audio_sec  = (uint32_t)audio.getAudioCurrentTime();

        if (!is_playing) {
            last_sync_tick = now_tick; 
        } else if (audio_sec != last_audio_sec) {
            last_audio_sec = audio_sec;
            last_sync_tick = now_tick;
        }

        const uint32_t sub_ms = (uint32_t)((now_tick - last_sync_tick) * portTICK_PERIOD_MS);
        const uint32_t current_ms = audio_sec * 1000u + sub_ms;

        next_line = -1; 

        for (int i = 0; i < lyric_count; i++) {
            if (current_ms >= current_lyrics[i].timestamp_ms) {
                next_line = i;
            } else {
                break; 
            }
        }

        if (next_line != last_drawn_line) {
            if (next_line == -1) {
                lcd.clear(); 
            } else {
                print_wrapped_to_lcd(current_lyrics[next_line].text, 0);
            }
            last_drawn_line = next_line;
        }
        current_line = next_line;
    }
}

void audio_eof_mp3(const char *info){
  Serial.printf("Song track execution completed: %s\n", info);
  system_state = STATE_MENU;
  is_playing = false;
  update_menu_lcd();
}

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
    while(true) 
      vTaskDelay(100);
  }

  scan_sd_for_songs();

  if (total_songs == 0) {
    lcd.clear();
    lcd.print("No MP3 files found!");
    while(true) 
      vTaskDelay(100);
  }

  play_pause_queue = xQueueCreate(1, sizeof(bool));
  volume_queue    = xQueueCreate(1, sizeof(int));
  change_track_queue = xQueueCreate(1, sizeof(int));

  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DIN);
  audio.setVolume(5);

  xTaskCreatePinnedToCore(audio_task, "Audio", 8192, NULL, 3, NULL, 1); 
  xTaskCreatePinnedToCore(ui_task,    "UI",    4096, NULL, 2, NULL, 0);        
  xTaskCreatePinnedToCore(lyric_task, "Lyrics", 4096, NULL, 1, NULL, 0); 

  vTaskDelete(NULL); 
}

void loop() {
}