/*
 * Copyright (c) 2025 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "led_widgets.h"

/*
 * LED Indicator Effects Definition
 * 
 * Behavior:
 * 1. USB Connected: Bright -> Gradually dim -> Off (bright for 0.5s, dim to off in 0.5s)
 * 2. BLE Connected: Off
 * 3. BLE Disconnected: Blink once every 1 second
 * 4. Low Battery (<20%): Double blink every 30 seconds
 */

const led_widget_t led_widgets[LED_EVENT_SIZE][CONFIG_ZMK_LED_WIDGETS_MAX_WIDGET_NUM] = {
    // LED_EVENT_USB
    {
        // Widget 0: USB Connected - Fade out effect
        // Priority: 50, No loop
        WIDGET(0, 50, 0, 9,
            CMD(50, 100),   // Full brightness, hold 50ms
            CMD(50, 90),    // Fade out
            CMD(50, 70),
            CMD(50, 50),
            CMD(50, 30),
            CMD(100, 10),   // Hold longer
            CMD(100, 5),
            CMD(100, 2),
            CMD(100, 0)     // Off
        ),
        
        // Widget 1: USB Disconnected - Keep off
        WIDGET(1, 10, 0, 1,
            CMD(0, 0)       // Off
        ),
    },
    
    // LED_EVENT_BLE
    {
        // Widget 0: BLE Connected - Off
        WIDGET(0, 40, 0, 1,
            CMD(0, 0)       // Off
        ),
        
        // Widget 1: BLE Disconnected - Blink every 1 second
        // Priority: 45, Loop period: 1000ms
        WIDGET(1, 45, 1000, 2,
            CMD(100, 100),  // On for 100ms
            WAIT(900)       // Wait 900ms
        ),
    },
    
    // LED_EVENT_BATTERY
    {
        // Widget 0: Normal battery - No indication
        WIDGET(0, 1, 0, 1,
            CMD(0, 0)       // Off
        ),
        
        // Widget 1: Low battery (<20%) - Double blink every 30 seconds
        // Priority: 100 (Highest priority)
        // Loop period: 30000ms
        WIDGET(1, 100, 30000, 6,
            CMD(100, 100),   // First blink, on for 100ms
            WAIT(100),       // Gap 100ms
            CMD(100, 100),   // Second blink, on for 100ms
            WAIT(29700),     // Wait remaining time
            CMD(0, 0),       // Ensure off
            WAIT(1)          // Brief delay
        ),
    },
};
