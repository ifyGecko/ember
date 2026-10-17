/*
 * uam/signal.h - The Signal interface: interrupt and GPIO lines.
 *
 * A signal is an output owned by its producer (a device). It is connected to
 * at most one sink: a function plus context plus an input number. The sink
 * is called only when the level changes. Producers do not know what they are
 * connected to; the board does the wiring.
 *
 *     uam_signal_connect(&uart->irq, uam_plic_input, plic, 10);
 *     uam_signal_set(&uart->irq, 1);
 *
 * Dependencies: none beyond common.h.
 */
#ifndef UAM_SIGNAL_H
#define UAM_SIGNAL_H

#include "uam/common.h"

typedef void (*uam_signal_sink)(void *ctx, unsigned input, int level);

typedef struct uam_signal {
    uam_signal_sink sink;
    void           *ctx;
    unsigned        input;
    int             level;
} uam_signal;

static inline void uam_signal_init(uam_signal *s)
{
    s->sink = NULL;
    s->ctx = NULL;
    s->input = 0;
    s->level = 0;
}

static inline void uam_signal_connect(uam_signal *s, uam_signal_sink sink, void *ctx, unsigned input)
{
    s->sink = sink;
    s->ctx = ctx;
    s->input = input;
    if (sink)
        sink(ctx, input, s->level);
}

static inline void uam_signal_set(uam_signal *s, int level)
{
    level = level != 0;
    if (level == s->level)
        return;
    s->level = level;
    if (s->sink)
        s->sink(s->ctx, s->input, level);
}

#endif /* UAM_SIGNAL_H */
