#include "beam_counter.h"

void beam_counter_init(beam_counter_t *c)
{
    c->first      = FIRST_NONE;
    c->t_first    = 0;
    c->prev_a     = false;
    c->prev_b     = false;
    c->wait_clear = false;
}

beam_event_t beam_counter_update(beam_counter_t *c, bool a, bool b, uint32_t now_ms)
{
    /* "cut" = the moment a beam goes from clear to blocked */
    bool a_cut = a && !c->prev_a;
    bool b_cut = b && !c->prev_b;
    c->prev_a = a;
    c->prev_b = b;

    /* 1. After a person was counted, ignore everything until both beams are clear. */
    if (c->wait_clear) {
        if (!a && !b) {
            c->wait_clear = false;
        }
        return BEAM_EVENT_NONE;
    }

    /* 2. Give up on a half-finished crossing (person turned back or stopped). */
    if (c->first != FIRST_NONE && (now_ms - c->t_first) > BEAM_PAIR_TIMEOUT_MS) {
        c->first = FIRST_NONE;
    }

    /* 3. Both beams cut in the same instant: direction unknown. Ignore. */
    if (a_cut && b_cut) {
        c->first      = FIRST_NONE;
        c->wait_clear = true;
        return BEAM_EVENT_NONE;
    }

    /* 4. The state machine. */
    if (c->first == FIRST_NONE) {
        /* Start a crossing only if the OTHER beam is not already blocked. */
        if (a_cut && !b) {
            c->first   = FIRST_A;
            c->t_first = now_ms;
        } else if (b_cut && !a) {
            c->first   = FIRST_B;
            c->t_first = now_ms;
        }
    } else if (c->first == FIRST_A) {
        if (b_cut) {                       /* A then B = IN */
            c->first      = FIRST_NONE;
            c->wait_clear = true;
            return BEAM_EVENT_ENTER;
        }
    } else { /* FIRST_B */
        if (a_cut) {                       /* B then A = OUT */
            c->first      = FIRST_NONE;
            c->wait_clear = true;
            return BEAM_EVENT_LEAVE;
        }
    }

    return BEAM_EVENT_NONE;
}