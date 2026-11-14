#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
void studio_radio_received(const uint8_t *data,size_t len);
void studio_radio_started(void);
void studio_radio_status(char *out,size_t size);
