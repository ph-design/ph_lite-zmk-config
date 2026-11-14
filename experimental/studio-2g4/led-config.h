#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
struct led_config { uint8_t mode, brightness, threshold; uint16_t period; };
static inline bool led_config_valid(const struct led_config *c) {
    return c->mode <= 2 && c->brightness <= 100 && c->threshold >= 5 &&
           c->threshold <= 50 && c->period >= 500 && c->period <= 5000;
}
static inline int led_config_parse(const char *args, struct led_config *c) {
    char mode[8]; unsigned int brightness, threshold, period; int used = 0;
    if (sscanf(args, "%7[A-Z],%u,%u,%u%n", mode, &brightness, &threshold, &period, &used) != 4 || args[used]) return -1;
    int id = !strcmp(mode,"AUTO") ? 0 : !strcmp(mode,"ON") ? 1 : !strcmp(mode,"OFF") ? 2 : -1;
    if (id < 0 || brightness > 100 || threshold < 5 || threshold > 50 || period < 500 || period > 5000) return -1;
    *c = (struct led_config){id, brightness, threshold, period};
    return 0;
}
