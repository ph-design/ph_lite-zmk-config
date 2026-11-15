#pragma once
#include <esb.h>
#define zmk_esb_payload esb_payload
int zmk_esb_write_payload(const struct esb_payload *p);
