/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NIKON_Z7_SHOULDER_H
#define NIKON_Z7_SHOULDER_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Independent observer state. Never used to drive guest-visible registers,
 * FIFO contents, ACKs or IRQs. Fixed-size framing is specific to Z7 C3.80.
 * Disable/config changes and a 100ms virtual gap discard partial messages. */
typedef struct Z7ShoulderObserver {
    bool enabled;
    unsigned count;
    int64_t last_ns;
    uint8_t frame[256];
} Z7ShoulderObserver;

static inline void z7_shoulder_reset(Z7ShoulderObserver *o)
{
    o->count = 0;
}

static inline bool z7_shoulder_word(Z7ShoulderObserver *o, unsigned spi,
                                    unsigned bits, uint16_t word, int64_t now)
{
    if (!o->enabled || spi != 0) {
        return false;
    }
    if (bits != 16) {
        z7_shoulder_reset(o);
        return false;
    }
    if (o->count && (now < o->last_ns || now - o->last_ns > 100000000)) {
        z7_shoulder_reset(o);
    }
    o->last_ns = now;
    o->frame[o->count++] = word & 0xff;
    o->frame[o->count++] = word >> 8;
    if (o->count != sizeof(o->frame)) {
        return false;
    }
    o->count = 0;
    const uint8_t *f = o->frame;
    uint8_t sum = 0;
    for (unsigned i = 0; i < sizeof(o->frame); i++) {
        sum += f[i];
    }
    return f[0] == 8 && f[2] == 112 && f[3] == 0 && f[8] == 4 &&
           f[252] >= f[253] && f[253] > 0 && sum == 255;
}
#endif
