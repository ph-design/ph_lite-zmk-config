/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
struct input_merge { uint8_t keyboard[2][15],consumer[2][12]; };
void merge_keyboard(const struct input_merge *s,uint8_t out[15]);
void merge_consumer(const struct input_merge *s,uint8_t out[24]);
