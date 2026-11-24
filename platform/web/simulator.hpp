// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

// Each module contains one sender profile. EEPROM contents survive sim_init().
extern "C" {
void sim_init();
int sim_step(int elapsed_ms);
void sim_set_analog(int index, int raw);
void sim_set_switch(int index, int position);
void sim_key(int key, int down);
void sim_trim(int button, int down);
// down < 0 cancels a stroke without recognizing a tap or swipe.
void sim_touch(int x, int y, int down);
std::uint8_t *sim_eeprom();
std::uint32_t sim_revision();
int sim_busy();
int sim_status();
const std::uint8_t *sim_lcd();
const std::int16_t *sim_channels();
const char *sim_error();
int sim_output();
}
