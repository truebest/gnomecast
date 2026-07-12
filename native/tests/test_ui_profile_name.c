#include "ui_profile_name.h"
#include "ui_slot_palette.h"

#include <assert.h>

static void test_supported_examples(void) {
    assert(native_ui_profile_name_valid("Office PC", 512u));
    assert(native_ui_profile_name_valid("caf\xc3\xa9", 512u));
    assert(native_ui_profile_name_valid("\xd0\x9a" "\xd0\xb0" "\xd0\xb1" "\xd0\xb8" "\xd0\xbd" "\xd0\xb5" "\xd1\x82", 512u));
}

static void test_unsupported_text_is_rejected(void) {
    assert(!native_ui_profile_name_valid("emoji \xf0\x9f\x96\xa5", 512u));
    assert(!native_ui_profile_name_valid("\xd1\xa0", 512u)); /* U+0460 is absent from IBM Plex. */
    assert(!native_ui_profile_name_valid("bad\xc3", 512u));
    assert(!native_ui_profile_name_valid("line\nbreak", 512u));
}

static void test_byte_capacity_is_enforced(void) {
    assert(native_ui_profile_name_valid("1234567", 8u));
    assert(!native_ui_profile_name_valid("12345678", 8u));
    assert(native_ui_profile_name_valid("\xd0\x9a" "\xd0\xb0", 5u));
    assert(!native_ui_profile_name_valid("\xd0\x9a" "\xd0\xb0", 4u));
}

static void test_declared_ranges(void) {
    for (uint32_t codepoint = 0x20u; codepoint <= 0x7eu; codepoint++) {
        assert(native_ui_profile_name_codepoint_supported(codepoint));
    }
    for (uint32_t codepoint = 0xa0u; codepoint <= 0xffu; codepoint++) {
        assert(native_ui_profile_name_codepoint_supported(codepoint));
    }
    for (uint32_t codepoint = 0x400u; codepoint <= 0x45fu; codepoint++) {
        assert(native_ui_profile_name_codepoint_supported(codepoint));
    }
    assert(!native_ui_profile_name_codepoint_supported(0x7fu));
    assert(!native_ui_profile_name_codepoint_supported(0x460u));
    assert(!native_ui_profile_name_codepoint_supported(0x1f5a5u));
    assert(native_ui_profile_name_valid(native_ui_profile_name_accepted_chars(), 800u));
}

static void test_slot_palette_mapping(void) {
    assert(native_ui_slot_rgb(NATIVE_SESSION_SLOT_RED) == NATIVE_UI_SLOT_RED_RGB);
    assert(native_ui_slot_rgb(NATIVE_SESSION_SLOT_GREEN) == NATIVE_UI_SLOT_GREEN_RGB);
    assert(native_ui_slot_rgb(NATIVE_SESSION_SLOT_YELLOW) == NATIVE_UI_SLOT_YELLOW_RGB);
    assert(native_ui_slot_rgb(NATIVE_SESSION_SLOT_BLUE) == NATIVE_UI_SLOT_BLUE_RGB);
}

int main(void) {
    test_supported_examples();
    test_unsupported_text_is_rejected();
    test_byte_capacity_is_enforced();
    test_declared_ranges();
    test_slot_palette_mapping();
    return 0;
}
