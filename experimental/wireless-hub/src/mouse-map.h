/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define MM_FIELDS 64
#define MM_REPORTS 16
enum mm_kind { MM_BUTTON, MM_X, MM_Y, MM_WHEEL, MM_PAN };
struct mm_field { uint16_t offset; uint8_t id, width, kind, button; bool sign; };
struct mm_report { uint16_t bits; uint8_t id; };
struct mouse_map {
    struct mm_field fields[MM_FIELDS];
    struct mm_report reports[MM_REPORTS];
    uint8_t field_count, report_count;
};
struct mouse_event { uint8_t buttons, button_mask; int32_t x,y,wheel,pan; };
int mouse_map_parse(struct mouse_map *map,const uint8_t *data,size_t len);
int mouse_map_decode(const struct mouse_map *map,uint8_t id,const uint8_t *data,
                     size_t len,struct mouse_event *out);
