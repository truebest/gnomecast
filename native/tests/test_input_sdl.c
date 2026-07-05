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

    assert(fake_call_count == 5);

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

static void test_linux_keycode_to_rdp(void) {
    uint8_t scancode = 0xff;
    bool extended = true;

    /* AT main block (Linux codes 1..88) is identity, not extended. */
    struct {
        uint32_t code;
        uint8_t rdp;
    } identity[] = {
        {1, 0x01},  /* KEY_ESC */
        {2, 0x02},  /* KEY_1 */
        {28, 0x1c}, /* KEY_ENTER */
        {30, 0x1e}, /* KEY_A */
        {57, 0x39}, /* KEY_SPACE */
        {59, 0x3b}, /* KEY_F1 */
        {63, 0x3f}, /* KEY_F5 */
        {76, 0x4c}, /* KEY_KP5 */
        {86, 0x56}, /* KEY_102ND */
        {87, 0x57}, /* KEY_F11 */
        {88, 0x58}, /* KEY_F12 */
    };
    for (size_t i = 0; i < sizeof(identity) / sizeof(identity[0]); i++) {
        scancode = 0xff;
        extended = true;
        assert(native_input_linux_keycode_to_rdp(identity[i].code, &scancode, &extended));
        assert(scancode == identity[i].rdp);
        assert(!extended);
    }

    /* E0-extended keys map via the explicit table. */
    struct {
        uint32_t code;
        uint8_t rdp;
    } ext[] = {
        {96, 0x1c},  /* KEY_KPENTER */
        {97, 0x1d},  /* KEY_RIGHTCTRL */
        {98, 0x35},  /* KEY_KPSLASH */
        {100, 0x38}, /* KEY_RIGHTALT */
        {102, 0x47}, /* KEY_HOME */
        {103, 0x48}, /* KEY_UP */
        {105, 0x4b}, /* KEY_LEFT */
        {106, 0x4d}, /* KEY_RIGHT */
        {108, 0x50}, /* KEY_DOWN */
        {110, 0x52}, /* KEY_INSERT */
        {111, 0x53}, /* KEY_DELETE */
        {125, 0x5b}, /* KEY_LEFTMETA */
        {127, 0x5d}, /* KEY_COMPOSE (Menu) */
    };
    for (size_t i = 0; i < sizeof(ext) / sizeof(ext[0]); i++) {
        scancode = 0xff;
        extended = false;
        assert(native_input_linux_keycode_to_rdp(ext[i].code, &scancode, &extended));
        assert(scancode == ext[i].rdp);
        assert(extended);
    }

    /* Codes with no RDP scancode: code 0, the extended-range gap (101 = KEY_LINEFEED,
     * 119 = KEY_PAUSE), and media/consumer keys above the table. */
    assert(!native_input_linux_keycode_to_rdp(0, &scancode, &extended));
    assert(!native_input_linux_keycode_to_rdp(101, &scancode, &extended));
    assert(!native_input_linux_keycode_to_rdp(119, &scancode, &extended));
    assert(!native_input_linux_keycode_to_rdp(240, &scancode, &extended));
}

int main(void) {
    test_mapping_and_inactive_guards();
    test_active_sends_values();
    test_sync_locks();
    test_linux_keycode_to_rdp();
    return 0;
}
