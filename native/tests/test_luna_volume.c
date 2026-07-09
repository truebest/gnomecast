#include "luna_volume.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    int volume = -1;
    bool muted = true;

    /* The exact reply shapes captured live from the TV (getVolume / master/getVolume). */
    assert(native_luna_volume_parse(
        "{\"returnValue\":true,\"muteStatus\":false,\"volume\":52,\"scenario\":\"mastervolume_ext_speaker_bt\"}",
        &volume, &muted));
    assert(volume == 52 && !muted);

    assert(native_luna_volume_parse("{\"returnValue\":true,\"volumeStatus\":{\"volumeLimitable\":true,"
                                    "\"maxVolume\":100,\"soundOutput\":\"bt_soundbar\",\"volume\":7,"
                                    "\"muteStatus\":true},\"callerId\":\"x\"}",
                                    &volume, &muted));
    assert(volume == 7 && muted);

    /* Subscription replies carry the same fields plus subscribed/action. */
    assert(native_luna_volume_parse("{\"subscribed\":true,\"returnValue\":true,\"action\":\"changed\","
                                    "\"muteStatus\":false,\"volume\":0}",
                                    &volume, &muted));
    assert(volume == 0 && !muted);
    assert(native_luna_volume_parse("{\"returnValue\":true,\"volume\":100}", &volume, NULL));
    assert(volume == 100);

    /* Failures must not report a volume: service errors, missing/garbled fields,
     * out-of-range values, empty replies (timeout produced nothing). */
    assert(!native_luna_volume_parse("{\"returnValue\":false,\"errorCode\":1,\"volume\":52}", &volume, &muted));
    assert(!native_luna_volume_parse("{\"returnValue\":true,\"muteStatus\":false}", &volume, &muted));
    assert(!native_luna_volume_parse("{\"returnValue\":true,\"volume\":}", &volume, &muted));
    assert(!native_luna_volume_parse("{\"returnValue\":true,\"volume\":101}", &volume, &muted));
    assert(!native_luna_volume_parse("{\"returnValue\":true,\"volume\":-3}", &volume, &muted));
    assert(!native_luna_volume_parse("", &volume, &muted));
    assert(!native_luna_volume_parse(NULL, &volume, &muted));

    printf("luna-volume: OK\n");
    return 0;
}
