/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
static inline int16_t hub_axis_fresh(uint32_t now,uint32_t time,int16_t value){return now-time<=250?value:0;}
