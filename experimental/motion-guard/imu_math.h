/* SPDX-License-Identifier: MIT */
#pragma once
#include <math.h>
static inline int16_t imu_axis(float angle, unsigned range, unsigned deadzone, bool invert) {
    float amount=fabsf(angle)-deadzone;
    if(amount<=0)return 0;
    float value=amount/(range-deadzone);if(value>1)value=1;
    return (int16_t)(value*32767*(angle<0?-1:1)*(invert?-1:1));
}
static inline void imu_angles(const float a[3], float *roll, float *pitch) {
    *roll=atan2f(a[1],a[2])*57.295779513f;
    *pitch=atan2f(-a[0],sqrtf(a[1]*a[1]+a[2]*a[2]))*57.295779513f;
}
static inline float imu_difference(float value,float center) {
    float d=value-center;while(d>180)d-=360;while(d<-180)d+=360;return d;
}
