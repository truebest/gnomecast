#include "input_sdl.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

struct RdpSession {
    uint32_t marker;
};

typedef enum FakeCallKind {
    FAKE_CALL_MOVE,
    FAKE_CALL_BUTTON,
    FAKE_CALL_WHEEL,
    FAKE_CALL_KEY,
    FAKE_CALL_UNICODE,
    FAKE_CALL_SYNC,
} FakeCallKind;

typedef struct FakeCall {
    FakeCallKind kind;
    RdpSession *session;
    uint16_t x;
    uint16_t y;
    uint8_t button;
    int16_t wheel_delta;
    uint8_t scancode;
    uint16_t codepoint;
    bool down;
    bool extended;
    bool scroll_lock;
    bool num_lock;
    bool caps_lock;
} FakeCall;

static FakeCall fake_calls[16];
static size_t fake_call_count;

static void fake_reset(void) {
    memset(fake_calls, 0, sizeof(fake_calls));
    fake_call_count = 0;
}

static FakeCall *fake_record(FakeCallKind kind, RdpSession *session) {
    assert(fake_call_count < sizeof(fake_calls) / sizeof(fake_calls[0]));
    FakeCall *call = &fake_calls[fake_call_count++];
    memset(call, 0, sizeof(*call));
    call->kind = kind;
    call->session = session;
    return call;
}

void rdp_send_pointer_move(RdpSession *session, uint16_t x, uint16_t y) {
    FakeCall *call = fake_record(FAKE_CALL_MOVE, session);
    call->x = x;
    call->y = y;
}

void rdp_send_pointer_button(RdpSession *session, uint16_t x, uint16_t y, uint8_t button, bool down) {
    FakeCall *call = fake_record(FAKE_CALL_BUTTON, session);
    call->x = x;
    call->y = y;
    call->button = button;
    call->down = down;
}

void rdp_send_pointer_wheel(RdpSession *session, uint16_t x, uint16_t y, int16_t delta) {
    FakeCall *call = fake_record(FAKE_CALL_WHEEL, session);
    call->x = x;
    call->y = y;
    call->wheel_delta = delta;
}

void rdp_send_key(RdpSession *session, uint8_t scancode, bool down, bool extended) {
    FakeCall *call = fake_record(FAKE_CALL_KEY, session);
    call->scancode = scancode;
    call->down = down;
    call->extended = extended;
}

void rdp_send_unicode(RdpSession *session, uint16_t codepoint, bool down) {
    FakeCall *call = fake_record(FAKE_CALL_UNICODE, session);
    call->codepoint = codepoint;
    call->down = down;
}

void rdp_send_sync(RdpSession *session, bool scroll_lock, bool num_lock, bool caps_lock) {
    FakeCall *call = fake_record(FAKE_CALL_SYNC, session);
    call->scroll_lock = scroll_lock;
    call->num_lock = num_lock;
    call->caps_lock = caps_lock;
}

static void assert_point(const NativeInput *input, int wx, int wy, uint16_t ex, uint16_t ey) {
    uint16_t x = 0;
    uint16_t y = 0;
    native_input_map_point(input, wx, wy, &x, &y);
    assert(x == ex);
    assert(y == ey);
}

static void test_mapping_and_inactive_guards(void) {
    NativeInput input;
    struct RdpSession session = {.marker = 0xfeedfaceu};
    native_input_init(&input, &session, 1920, 1080);
    native_input_set_window_size(&input, 960, 540);

    assert_point(&input, 0, 0, 0, 0);
    assert_point(&input, 480, 270, 960, 540);
    assert_point(&input, 959, 539, 1918, 1078);
    assert_point(&input, -50, -20, 0, 0);
    assert_point(&input, 2000, 2000, 1918, 1078);

    native_input_set_window_size(&input, 3840, 2160);
    assert_point(&input, 0, 0, 0, 0);
    assert_point(&input, 1920, 1080, 960, 540);
    assert_point(&input, 3839, 2159, 1919, 1079);

    native_input_set_desktop_size(&input, 0, 0);
    native_input_set_window_size(&input, 0, 0);
    assert_point(&input, 100, 100, 0, 0);

    fake_reset();
    input.session = NULL;
    native_input_set_active(&input, true);
    assert(!native_input_pointer_move(&input, 1, 1));
    native_input_set_session(&input, &session);
    native_input_set_active(&input, false);
    assert(!native_input_pointer_move(&input, 1, 1));
    assert(!native_input_pointer_button(&input, 1, 1, NATIVE_INPUT_BUTTON_LEFT, true));
    assert(!native_input_pointer_wheel(&input, 1, 1, 120));
    assert(!native_input_key(&input, 0x1e, true, false));
    assert(!native_input_unicode(&input, 0x0041, true));
    assert(fake_call_count == 0);
}

static void test_active_sends_values(void) {
    NativeInput input;
    struct RdpSession session = {.marker = 0xcafebabeu};
    native_input_init(&input, &session, 1920, 1080);
    native_input_set_window_size(&input, 960, 540);
    native_input_set_active(&input, true);

    fake_reset();
    assert(native_input_pointer_move(&input, 480, 270));
    assert(native_input_pointer_button(&input, 959, 539, NATIVE_INPUT_BUTTON_RIGHT, true));
    assert(native_input_pointer_wheel(&input, -20, 2000, -120));
    assert(native_input_key(&input, 0x1e, true, false));
    assert(native_input_key(&input, 0x1d, false, true));
    assert(native_input_unicode(&input, 0x00e9, true));

    assert(fake_call_count == 6);

    assert(fake_calls[0].kind == FAKE_CALL_MOVE);
    assert(fake_calls[0].session == &session);
    assert(fake_calls[0].x == 960);
    assert(fake_calls[0].y == 540);

    assert(fake_calls[1].kind == FAKE_CALL_BUTTON);
    assert(fake_calls[1].session == &session);
    assert(fake_calls[1].x == 1918);
    assert(fake_calls[1].y == 1078);
    assert(fake_calls[1].button == NATIVE_INPUT_BUTTON_RIGHT);
    assert(fake_calls[1].down);

    assert(fake_calls[2].kind == FAKE_CALL_WHEEL);
    assert(fake_calls[2].session == &session);
    assert(fake_calls[2].x == 0);
    assert(fake_calls[2].y == 1078);
    assert(fake_calls[2].wheel_delta == -120);

    assert(fake_calls[3].kind == FAKE_CALL_KEY);
    assert(fake_calls[3].session == &session);
    assert(fake_calls[3].scancode == 0x1e);
    assert(fake_calls[3].down);
    assert(!fake_calls[3].extended);

    assert(fake_calls[4].kind == FAKE_CALL_KEY);
    assert(fake_calls[4].session == &session);
    assert(fake_calls[4].scancode == 0x1d);
    assert(!fake_calls[4].down);
    assert(fake_calls[4].extended);

    assert(fake_calls[5].kind == FAKE_CALL_UNICODE);
    assert(fake_calls[5].session == &session);
    assert(fake_calls[5].codepoint == 0x00e9);
    assert(fake_calls[5].down);
}

static void assert_scancode(uint32_t sdl_scancode, uint8_t expected, bool expected_extended) {
    uint8_t scancode = 0;
    bool extended = false;
    assert(native_input_sdl_scancode_to_rdp(sdl_scancode, &scancode, &extended));
    assert(scancode == expected);
    assert(extended == expected_extended);
}

static void test_keypad_scancode_mapping(void) {
    /* SDL keypad scancodes run KP_1..KP_9 (89..97) then KP_0 (98). */
    assert_scancode(89, 0x4f, false); /* KP_1 */
    assert_scancode(90, 0x50, false); /* KP_2 */
    assert_scancode(91, 0x51, false); /* KP_3 */
    assert_scancode(92, 0x4b, false); /* KP_4 */
    assert_scancode(93, 0x4c, false); /* KP_5 */
    assert_scancode(94, 0x4d, false); /* KP_6 */
    assert_scancode(95, 0x47, false); /* KP_7 */
    assert_scancode(96, 0x48, false); /* KP_8 */
    assert_scancode(97, 0x49, false); /* KP_9 */
    assert_scancode(98, 0x52, false); /* KP_0 */
    assert_scancode(99, 0x53, false); /* KP_period */
    assert_scancode(83, 0x45, false); /* Num Lock */
    assert_scancode(84, 0x35, true);  /* KP_divide */
    assert_scancode(85, 0x37, false); /* KP_multiply */
    assert_scancode(86, 0x4a, false); /* KP_minus */
    assert_scancode(87, 0x4e, false); /* KP_plus */
    assert_scancode(88, 0x1c, true);  /* KP_enter */
}

static void assert_unmapped(uint32_t sdl_scancode) {
    uint8_t scancode = 0xa5;
    bool extended = true;
    assert(!native_input_sdl_scancode_to_rdp(sdl_scancode, &scancode, &extended));
}

static void test_webos_pseudo_keys_unmapped(void) {
    /* LG's webOS SDL delivers TV/Magic Remote pseudo-keys as SDL scancodes in
     * the 300..511 range: 484/485 are cursor show/hide (observed in live logs
     * whenever the pointer visibility changes, as key-down-only events with
     * sym=0), 486 is the remote red key, 480/481 channel up/down, 505 exit.
     * None of these are keyboard input and none may ever be forwarded to the
     * RDP server as a keyboard scancode. */
    for (uint32_t sdl_scancode = 300; sdl_scancode < 512; sdl_scancode++) {
        assert_unmapped(sdl_scancode);
    }
    assert_unmapped(484); /* SDL_WEBOS_SCANCODE_CURSOR_SHOW */
    assert_unmapped(485); /* SDL_WEBOS_SCANCODE_CURSOR_HIDE */
    assert_unmapped(486); /* SDL_WEBOS_SCANCODE_RED */
}

static void test_sync_locks(void) {
    NativeInput input;
    struct RdpSession session = {.marker = 0xabad1deau};
    native_input_init(&input, &session, 1920, 1080);

    fake_reset();
    assert(!native_input_sync_locks(&input, false, true, false));
    assert(fake_call_count == 0);

    native_input_set_active(&input, true);
    assert(native_input_sync_locks(&input, false, true, true));
    assert(fake_call_count == 1);
    assert(fake_calls[0].kind == FAKE_CALL_SYNC);
    assert(fake_calls[0].session == &session);
    assert(!fake_calls[0].scroll_lock);
    assert(fake_calls[0].num_lock);
    assert(fake_calls[0].caps_lock);
}

static void test_motion_jump_filter(void) {
    /* Coarse net: only deltas no physical device produces in one event. */
    assert(!native_input_motion_is_jump(5, -3));
    assert(!native_input_motion_is_jump(NATIVE_INPUT_JUMP_ALWAYS_PX, 0));
    assert(native_input_motion_is_jump(NATIVE_INPUT_JUMP_ALWAYS_PX + 1, 0));
    assert(native_input_motion_is_jump(-(NATIVE_INPUT_JUMP_ALWAYS_PX + 1), 12));

    /* Center-jump signature: large delta landing at the window center (1920x1080 -> 960,540). */
    assert(native_input_motion_is_center_jump(960, 540, 300, -200, 1920, 1080));
    assert(native_input_motion_is_center_jump(960 + NATIVE_INPUT_CENTER_TOLERANCE_PX, 540,
                                              NATIVE_INPUT_CENTER_JUMP_MIN_PX, 0, 1920, 1080));

    /* A small final delta ending at the center is a legitimate glide, not a warp. */
    assert(!native_input_motion_is_center_jump(960, 540, NATIVE_INPUT_CENTER_JUMP_MIN_PX - 1, 3,
                                               1920, 1080));
    /* A large delta landing away from the center is a legitimate fast move. */
    assert(!native_input_motion_is_center_jump(700, 540, 300, 0, 1920, 1080));
    /* Warp coalesced with concurrent movement: lands near (not at) the center with a
     * delta no real per-event motion reaches. */
    assert(native_input_motion_is_center_jump(960 + NATIVE_INPUT_CENTER_RADIUS_PX, 540,
                                              NATIVE_INPUT_CENTER_RADIUS_JUMP_PX, 0, 1920, 1080));
    assert(!native_input_motion_is_center_jump(960 + NATIVE_INPUT_CENTER_RADIUS_PX + 1, 540,
                                               NATIVE_INPUT_CENTER_RADIUS_JUMP_PX, 0, 1920, 1080));
    assert(!native_input_motion_is_center_jump(960 + NATIVE_INPUT_CENTER_RADIUS_PX, 540,
                                               NATIVE_INPUT_CENTER_RADIUS_JUMP_PX - 1, 0, 1920,
                                               1080));
    /* Near-center but small delta stays a legitimate move (tolerance tier needs 40+). */
    assert(!native_input_motion_is_center_jump(960, 540 + NATIVE_INPUT_CENTER_TOLERANCE_PX + 1,
                                               NATIVE_INPUT_CENTER_JUMP_MIN_PX, 0, 1920, 1080));
    /* Degenerate window sizes never match. */
    assert(!native_input_motion_is_center_jump(0, 0, 300, 0, 0, 0));
}

int main(void) {
    test_mapping_and_inactive_guards();
    test_active_sends_values();
    test_keypad_scancode_mapping();
    test_webos_pseudo_keys_unmapped();
    test_sync_locks();
    test_motion_jump_filter();
    return 0;
}
