/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
/* Datasheet sensitivity: hundredths of mg/digit in 8-bit low-power mode.
 * Normal (10 bit) divides sensitivity by 4; HR (12 bit) divides by 16. */
static inline int64_t lis2dh_decode_micro_ms2(int16_t raw, uint32_t sensitivity, unsigned shift) {
    uint32_t bits = (uint16_t)raw >> shift;
    int32_t digit = (int32_t)bits;
    if (bits & (1U << (15 - shift))) digit -= (int32_t)(1U << (16 - shift));
    return (int64_t)digit * 9806650LL * sensitivity / (100000LL * (1U << (8 - shift)));
}
static inline int16_t lis2dh_decode_axis(const uint8_t *bytes) {
    uint16_t bits = (uint16_t)bytes[0] | (uint16_t)bytes[1] << 8;
    return bits & 0x8000 ? (int16_t)((int32_t)bits - 65536) : (int16_t)bits;
}
