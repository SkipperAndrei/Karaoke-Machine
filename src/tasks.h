#ifndef __TASKS_H__
#define __TASKS_H__

#include "utils.h"

// extern Audio audio;
// extern LiquidCrystal_I2C lcd(0x27, 20, 4);

enum State {
    PICK,
    PLAY
};

// extern uint8_t current_state;

void menu_task(void *params);
void lyric_task(void *params);
void audio_task(void* params);

#endif