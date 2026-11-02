#ifndef INDICATOR_H
#define INDICATOR_H

#include <stdbool.h>

typedef enum {
    LED_GREEN,      /* access granted            */
    LED_RED,        /* denied / alarm            */
    LED_YELLOW,     /* door open, waiting beam   */
} led_t;

/* Set up the 3 LEDs and the buzzer. */
void indicators_init(void);

void led_set(led_t led, bool on);

void buzzer_set(bool on);

/* Beep for the given time (waits until finished). */
void buzzer_beep(int ms);

#endif