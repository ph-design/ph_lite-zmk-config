#pragma once
#include <stddef.h>
int ble_mouse_init(void);
int ble_mouse_pair(void);
int ble_mouse_connect(unsigned index);
int ble_mouse_clear(void);
int ble_mouse_resume(void);
void ble_mouse_status(char *out,size_t size);
