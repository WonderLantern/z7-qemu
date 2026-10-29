/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <stdio.h>
#include "../qemu/nikon-z7-shoulder.h"

static unsigned feed(Z7ShoulderObserver *o, uint8_t *f, unsigned begin,
                     unsigned end, unsigned spi, unsigned bits, int64_t now)
{
    unsigned hits = 0;
    for (unsigned i = begin; i < end; i += 2) {
        hits += z7_shoulder_word(o, spi, bits, f[i] | f[i + 1] << 8, now);
    }
    return hits;
}

static void checksum(uint8_t *f)
{
    uint8_t sum = 0;
    for (unsigned i = 0; i < 255; i++) { sum += f[i]; }
    f[255] = 255 - sum;
}

int main(void)
{
    uint8_t frame[256] = {0};
    frame[0] = 8; frame[2] = 112; frame[8] = 4;
    frame[252] = frame[253] = 1;
    frame[255] = (uint8_t)(255 - 8 - 112 - 4 - 2);
    Z7ShoulderObserver o = {0};
    assert(feed(&o, frame, 0, 256, 0, 16, 0) == 0);
    o.enabled = true;
    assert(feed(&o, frame, 0, 128, 0, 16, 0) == 0);
    assert(feed(&o, frame, 128, 256, 0, 16, 100000) == 1);
    assert(memcmp(o.frame, frame, 256) == 0);
    assert(feed(&o, frame, 0, 256, 1, 16, 0) == 0);
    assert(feed(&o, frame, 0, 256, 0, 8, 0) == 0);
    assert(feed(&o, frame, 0, 128, 0, 16, 0) == 0);
    assert(feed(&o, frame, 128, 256, 0, 16, 100000001) == 0);
    z7_shoulder_reset(&o);
    assert(feed(&o, frame, 0, 128, 0, 16, 0) == 0);
    z7_shoulder_reset(&o);
    assert(feed(&o, frame, 0, 256, 0, 16, 0) == 1);
    assert(feed(&o, frame, 0, 256, 0, 16, 0) == 1); /* duplicate observation */
    frame[2] = 111; checksum(frame);
    assert(feed(&o, frame, 0, 256, 0, 16, 0) == 0);
    frame[2] = 112; frame[0] = 7; checksum(frame);
    assert(feed(&o, frame, 0, 256, 0, 16, 0) == 0);
    frame[0] = 8;
    frame[8] = 5; checksum(frame);
    assert(feed(&o, frame, 0, 256, 0, 16, 0) == 0);
    frame[8] = 4; checksum(frame); frame[200] = 1;
    assert(feed(&o, frame, 0, 256, 0, 16, 0) == 0);
    puts("PASS synthetic passive shoulder observer contracts");
}
