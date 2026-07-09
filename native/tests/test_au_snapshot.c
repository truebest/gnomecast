#include "au_snapshot.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fill(uint8_t *buf, size_t len, uint8_t seed) {
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(seed + i);
    }
}

static void test_unarmed_and_basic_append(void) {
    NativeAuSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    assert(!native_au_snapshot_ready(&snap));

    uint8_t idr[16];
    fill(idr, sizeof(idr), 1);
    /* Unarmed appends are rejected without effect. */
    assert(!native_au_snapshot_append(&snap, idr, sizeof(idr), true, 0));
    assert(snap.count == 0 && snap.buf == NULL);

    assert(native_au_snapshot_arm(&snap));
    assert(snap.armed && snap.buf != NULL && !native_au_snapshot_ready(&snap));

    /* Deltas before the first keyframe are skipped but do not invalidate. */
    uint8_t delta[8];
    fill(delta, sizeof(delta), 7);
    assert(native_au_snapshot_append(&snap, delta, sizeof(delta), false, 100));
    assert(snap.count == 0 && !native_au_snapshot_ready(&snap));

    /* Empty pushes are ignored, valid either way. */
    assert(native_au_snapshot_append(&snap, NULL, 0, false, 0));
    assert(snap.count == 0);

    /* Keyframe seeds the snapshot; a delta appends after it. */
    assert(native_au_snapshot_append(&snap, idr, sizeof(idr), true, 200));
    assert(native_au_snapshot_ready(&snap));
    assert(native_au_snapshot_append(&snap, delta, sizeof(delta), false, 300));
    assert(snap.count == 2 && snap.used == sizeof(idr) + sizeof(delta));
    assert(snap.entries[0].is_keyframe && snap.entries[0].len == sizeof(idr) && snap.entries[0].pts90k == 200);
    assert(!snap.entries[1].is_keyframe && snap.entries[1].len == sizeof(delta) && snap.entries[1].pts90k == 300);
    assert(memcmp(snap.buf + snap.entries[0].offset, idr, sizeof(idr)) == 0);
    assert(memcmp(snap.buf + snap.entries[1].offset, delta, sizeof(delta)) == 0);

    native_au_snapshot_reset(&snap);
    assert(snap.buf == NULL && !snap.armed && !native_au_snapshot_ready(&snap));
    /* Double reset is safe. */
    native_au_snapshot_reset(&snap);
}

static void test_keyframe_restarts(void) {
    NativeAuSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    assert(native_au_snapshot_arm(&snap));

    uint8_t first[32], tail[4], second[24];
    fill(first, sizeof(first), 10);
    fill(tail, sizeof(tail), 20);
    fill(second, sizeof(second), 30);

    assert(native_au_snapshot_append(&snap, first, sizeof(first), true, 1));
    assert(native_au_snapshot_append(&snap, tail, sizeof(tail), false, 2));
    assert(native_au_snapshot_append(&snap, tail, sizeof(tail), false, 3));
    assert(snap.count == 3);

    /* A newer keyframe drops everything before it: only the fresh IDR group matters. */
    assert(native_au_snapshot_append(&snap, second, sizeof(second), true, 4));
    assert(native_au_snapshot_ready(&snap));
    assert(snap.count == 1 && snap.used == sizeof(second));
    assert(snap.entries[0].is_keyframe && snap.entries[0].offset == 0 && snap.entries[0].pts90k == 4);
    assert(memcmp(snap.buf, second, sizeof(second)) == 0);

    native_au_snapshot_reset(&snap);
}

static void test_entry_overflow_invalidates(void) {
    NativeAuSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    assert(native_au_snapshot_arm(&snap));

    uint8_t au[4];
    fill(au, sizeof(au), 5);
    assert(native_au_snapshot_append(&snap, au, sizeof(au), true, 0));
    for (unsigned i = 1; i < NATIVE_AU_SNAPSHOT_MAX_AUS; i++) {
        assert(native_au_snapshot_append(&snap, au, sizeof(au), false, i));
    }
    assert(snap.count == NATIVE_AU_SNAPSHOT_MAX_AUS);

    /* One entry past the cap voids the snapshot: a partial chain is undecodable. */
    assert(!native_au_snapshot_append(&snap, au, sizeof(au), false, 999));
    assert(!snap.armed && !native_au_snapshot_ready(&snap) && snap.buf == NULL);
}

static void test_byte_overflow_invalidates(void) {
    NativeAuSnapshot snap;
    memset(&snap, 0, sizeof(snap));
    uint8_t *big = (uint8_t *)malloc(NATIVE_AU_SNAPSHOT_MAX_BYTES);
    assert(big);
    memset(big, 0xAB, NATIVE_AU_SNAPSHOT_MAX_BYTES);

    /* A keyframe of exactly the cap fits... */
    assert(native_au_snapshot_arm(&snap));
    assert(native_au_snapshot_append(&snap, big, NATIVE_AU_SNAPSHOT_MAX_BYTES, true, 0));
    assert(native_au_snapshot_ready(&snap) && snap.used == NATIVE_AU_SNAPSHOT_MAX_BYTES);
    /* ...and the next byte voids the snapshot. */
    uint8_t one = 0xCD;
    assert(!native_au_snapshot_append(&snap, &one, 1, false, 1));
    assert(!snap.armed && snap.buf == NULL);

    /* A delta that would cross the cap voids the snapshot too. */
    assert(native_au_snapshot_arm(&snap));
    assert(native_au_snapshot_append(&snap, big, NATIVE_AU_SNAPSHOT_MAX_BYTES - 1, true, 0));
    assert(!native_au_snapshot_append(&snap, big, 2, false, 1));
    assert(!snap.armed && snap.buf == NULL);

    native_au_snapshot_reset(&snap);
    free(big);
}

int main(void) {
    test_unarmed_and_basic_append();
    test_keyframe_restarts();
    test_entry_overflow_invalidates();
    test_byte_overflow_invalidates();
    printf("au-snapshot: OK\n");
    return 0;
}
