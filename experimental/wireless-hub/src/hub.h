#pragma once
#include <stdint.h>
#include <stddef.h>
#include "mouse-map.h"
int hub_usb_init(void);
int hub_usb_report(uint8_t id,const uint8_t *data,size_t len);
void hub_mouse_event(unsigned source,const struct mouse_event *event);
void hub_mouse_release(unsigned source);
void hub_radio_run(void);
int hub_radio_pause(bool pause);
extern int hub_radio_error;
