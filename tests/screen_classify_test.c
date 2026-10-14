#include "jpeg_dc.h"
#include "screen_classify.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fixtures are real console screenshots, scaled down to keep them small; the
 * regions the classifier measures are relative, so the verdicts do not depend on
 * that scaling. */
static ScreenClass classify(const char* path, ScreenMetrics* metrics)
{
    FILE* file = fopen(path, "rb");
    assert(file != NULL);
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    unsigned char* data = malloc((size_t)size);
    assert(data != NULL);
    assert(fread(data, 1, (size_t)size, file) == (size_t)size);
    fclose(file);

    JpegDcImage* image = malloc(sizeof(*image));
    assert(image != NULL);
    bool ok = jpegDcDecode(data, (size_t)size, image);
    assert(ok);
    ScreenClass verdict = screenClassify(image, metrics);
    free(image);
    free(data);
    return verdict;
}

int main(void)
{
    ScreenMetrics metrics;

    /* Both themes of the plain lock screen: a flat right side plus the busy news
     * panel on the left. */
    assert(classify("tests/fixtures/lock-one-light.jpg", &metrics) == ScreenLockOne);
    assert(metrics.rightFlat >= 0.65f && metrics.rightSpread <= 24.0f);
    assert(metrics.leftFlat < 0.65f);
    assert(classify("tests/fixtures/lock-one-dark.jpg", &metrics) == ScreenLockOne);
    assert(metrics.rightFlat >= 0.65f && metrics.rightSpread <= 24.0f);

    /* The Screen-Lock variant wants three presses: flat across the whole screen. */
    assert(classify("tests/fixtures/lock-three-light.jpg", &metrics) == ScreenLockThree);
    assert(metrics.rightFlat >= 0.65f && metrics.leftFlat >= 0.65f);
    assert(classify("tests/fixtures/lock-three-dark.jpg", &metrics) == ScreenLockThree);
    assert(metrics.rightFlat >= 0.65f && metrics.leftFlat >= 0.65f);

    /* The HOME menu is busy where the lock screen is flat. */
    assert(classify("tests/fixtures/home-menu.jpg", &metrics) == ScreenOther);
    assert(metrics.rightFlat < 0.65f);

    /* Anything that is not a baseline JPEG is "unknown", never "a lock screen". */
    JpegDcImage* image = malloc(sizeof(*image));
    assert(image != NULL);
    const unsigned char notAJpeg[] = { 'n', 'o', 'p', 'e', 0x00, 0xFF, 0x12, 0x34 };
    assert(!jpegDcDecode(notAJpeg, sizeof(notAJpeg), image));
    assert(screenClassify(image, &metrics) == ScreenUnknown);
    free(image);

    puts("screen_classify_test: ok");
    return 0;
}
