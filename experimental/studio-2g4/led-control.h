#pragma once
#include <stddef.h>
void dongle_led_status(char *out, size_t size);
int dongle_led_configure(const char *args);
int dongle_led_save(void);
void dongle_led_identify(void);
