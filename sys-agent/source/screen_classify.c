#include "screen_classify.h"

#include <math.h>
#include <string.h>

/* Regions are relative, so they do not care about the frame's size or about a
 * fixture having been scaled while the thresholds were tuned. The right region
 * is the flat half of the lock screen (under the status bar, above the hint
 * bar); the left region is where the plain lock screen puts its news panel. */
#define RIGHT_X0 0.42f
#define RIGHT_X1 0.97f
#define LEFT_X0  0.02f
#define LEFT_X1  0.27f
#define REGION_Y0 0.15f
#define REGION_Y1 0.80f
#define GRID_COLS 8
#define GRID_ROWS 5

/* Measured on the development console's frames (160x90 luma plane):
 *   lock screen (light/dark, "开始"/"继续"): rightFlat 0.80, rightSpread 0.0
 *   lock screen, three-press variant:        rightFlat 0.80, leftFlat 1.00
 *   HOME menu:                               rightFlat 0.25
 *   "who is playing?" dialog:                rightFlat 0.50, rightSpread 134
 * The two conditions are independent on purpose: the flat ratio rejects busy
 * screens, the spread rejects screens that are flat but multi-coloured. */
#define FLAT_STDDEV 8.0f
#define FLAT_RATIO_MIN 0.65f
#define SPREAD_MAX 24.0f
/* A frame this dark is not a screen with content: capssc usually fails outright
 * while the display is off, but a very dark frame is treated the same way. */
#define DARK_MEAN_MAX 8.0f

typedef struct {
    float flatRatio;
    float spread;
    float mean;
} RegionStats;

static RegionStats measureRegion(const JpegDcImage* image, float x0f, float x1f)
{
    RegionStats stats = { 0.0f, 0.0f, 0.0f };
    int x0 = (int)(x0f * (float)image->blocksW);
    int x1 = (int)(x1f * (float)image->blocksW);
    int y0 = (int)(REGION_Y0 * (float)image->blocksH);
    int y1 = (int)(REGION_Y1 * (float)image->blocksH);
    if (x1 <= x0 || y1 <= y0)
        return stats;

    int blockW = (x1 - x0) / GRID_COLS;
    int blockH = (y1 - y0) / GRID_ROWS;
    if (blockW < 1) blockW = 1;
    if (blockH < 1) blockH = 1;

    float means[GRID_COLS * GRID_ROWS];
    bool flat[GRID_COLS * GRID_ROWS];
    int total = 0;
    float sum = 0.0f;
    for (int row = 0; row < GRID_ROWS; row++) {
        for (int col = 0; col < GRID_COLS; col++) {
            float blockSum = 0.0f, blockSquareSum = 0.0f;
            int count = 0;
            for (int y = y0 + row * blockH; y < y0 + (row + 1) * blockH && y < y1; y++) {
                for (int x = x0 + col * blockW; x < x0 + (col + 1) * blockW && x < x1; x++) {
                    float value = (float)image->mean[y][x];
                    blockSum += value;
                    blockSquareSum += value * value;
                    count++;
                }
            }
            if (count == 0)
                continue;
            float mean = blockSum / (float)count;
            float variance = blockSquareSum / (float)count - mean * mean;
            if (variance < 0.0f)
                variance = 0.0f;
            means[total] = mean;
            flat[total] = sqrtf(variance) < FLAT_STDDEV;
            total++;
            sum += mean;
        }
    }
    if (total == 0)
        return stats;

    stats.mean = sum / (float)total;
    float flatMin = 0.0f, flatMax = 0.0f;
    int flatCount = 0;
    for (int i = 0; i < total; i++) {
        if (!flat[i])
            continue;
        if (flatCount == 0 || means[i] < flatMin)
            flatMin = means[i];
        if (flatCount == 0 || means[i] > flatMax)
            flatMax = means[i];
        flatCount++;
    }
    stats.flatRatio = (float)flatCount / (float)total;
    stats.spread = flatCount ? flatMax - flatMin : 0.0f;
    return stats;
}

ScreenClass screenClassify(const JpegDcImage* image, ScreenMetrics* metrics)
{
    ScreenMetrics local;
    memset(&local, 0, sizeof(local));
    if (metrics == NULL)
        metrics = &local;
    if (image == NULL || image->blocksW <= 0 || image->blocksH <= 0) {
        /* Metrics stay empty: there was no frame to measure. */
        memset(metrics, 0, sizeof(*metrics));
        return ScreenUnknown;
    }

    RegionStats right = measureRegion(image, RIGHT_X0, RIGHT_X1);
    RegionStats left = measureRegion(image, LEFT_X0, LEFT_X1);
    metrics->rightFlat = right.flatRatio;
    metrics->rightSpread = right.spread;
    metrics->rightMean = right.mean;
    metrics->leftFlat = left.flatRatio;
    metrics->leftSpread = left.spread;

    if (!image->complete)
        return ScreenUnknown;
    if (right.mean <= DARK_MEAN_MAX)
        return ScreenDark;
    if (right.flatRatio < FLAT_RATIO_MIN || right.spread > SPREAD_MAX)
        return ScreenOther;
    return left.flatRatio >= FLAT_RATIO_MIN ? ScreenLockThree : ScreenLockOne;
}

const char* screenClassName(ScreenClass value)
{
    switch (value) {
        case ScreenDark: return "dark";
        case ScreenLockOne: return "lock-one";
        case ScreenLockThree: return "lock-three";
        case ScreenOther: return "other";
        case ScreenUnknown:
        default: return "unknown";
    }
}
