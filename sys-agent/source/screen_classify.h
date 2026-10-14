#pragma once

#include <stdbool.h>

#include "jpeg_dc.h"

/* What the boot unlock is looking at. The lock screen is recognised by its
 * picture, not by any system flag: it keeps a large flat area on the right, and
 * its two variants differ on the left (the plain one has the news panel there,
 * the Screen-Lock one is flat across the whole screen). */
typedef enum {
    ScreenUnknown = 0,   /* the frame did not decode */
    ScreenDark,          /* a frame that is black: nothing is being displayed */
    ScreenLockOne,       /* lock screen that opens with one press */
    ScreenLockThree,     /* lock screen that wants the same button three times */
    ScreenOther          /* anything else - the HOME menu, a dialog, a game */
} ScreenClass;

typedef struct {
    float rightFlat;        /* 0..1 of the sampled blocks in the right region */
    float rightSpread;      /* brightness spread of those flat blocks */
    float rightMean;
    float leftFlat;         /* same for the left region (the news panel) */
    float leftSpread;
} ScreenMetrics;

/* Classifies one decoded frame. Metrics are always filled in (also for
 * ScreenUnknown) so a report can show why the verdict came out that way. */
ScreenClass screenClassify(const JpegDcImage* image, ScreenMetrics* metrics);
const char* screenClassName(ScreenClass value);
