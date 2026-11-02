#ifndef BEAM_COUNTER_H
#define BEAM_COUNTER_H

#include <stdbool.h>
#include <stdint.h>

/* How long after the FIRST beam is cut we wait for the SECOND beam. */
#define BEAM_PAIR_TIMEOUT_MS 2000

typedef enum {
    BEAM_EVENT_NONE,   /* nothing happened */
    BEAM_EVENT_ENTER,  /* beam A then beam B: one person came in  */
    BEAM_EVENT_LEAVE   /* beam B then beam A: one person went out */
} beam_event_t;

typedef enum {
    FIRST_NONE,        /* waiting for someone */
    FIRST_A,           /* beam A was cut first, waiting for B */
    FIRST_B            /* beam B was cut first, waiting for A */
} beam_first_t;

typedef struct {
    beam_first_t first;      /* which beam was cut first */
    uint32_t     t_first;    /* when (ms) it was cut */
    bool         prev_a;     /* previous state of beam A */
    bool         prev_b;     /* previous state of beam B */
    bool         wait_clear; /* after a count, both beams must clear first */
} beam_counter_t;

void         beam_counter_init(beam_counter_t *c);

/* Call every few milliseconds with the CLEAN beam states.
 * a, b  : true while that beam is cut (blocked)
 * now_ms: current time in milliseconds */
beam_event_t beam_counter_update(beam_counter_t *c, bool a, bool b, uint32_t now_ms);

#endif