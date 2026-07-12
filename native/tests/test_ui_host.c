#include "ui_host.h"

#include <assert.h>
#include <string.h>

static void test_bare_hosts_are_preserved(void) {
    char host[256];
    assert(native_ui_host_normalize("desktop.example", host, sizeof(host)));
    assert(strcmp(host, "desktop.example") == 0);
    assert(native_ui_host_normalize("2001:db8::1", host, sizeof(host)));
    assert(strcmp(host, "2001:db8::1") == 0);
}

static void test_bracketed_ipv6_is_unwrapped(void) {
    char host[256];
    assert(native_ui_host_normalize("[2001:db8::1]", host, sizeof(host)));
    assert(strcmp(host, "2001:db8::1") == 0);
    assert(native_ui_host_normalize("[::ffff:192.0.2.1]", host, sizeof(host)));
    assert(strcmp(host, "::ffff:192.0.2.1") == 0);
}

static void test_malformed_brackets_are_rejected(void) {
    char host[256];
    assert(!native_ui_host_normalize("[2001:db8::1", host, sizeof(host)));
    assert(!native_ui_host_normalize("2001:db8::1]", host, sizeof(host)));
    assert(!native_ui_host_normalize("[[2001:db8::1]]", host, sizeof(host)));
    assert(!native_ui_host_normalize("[2001:::1]", host, sizeof(host)));
    assert(!native_ui_host_normalize("[::1]:3389", host, sizeof(host)));
    assert(!native_ui_host_normalize("foo[bar]", host, sizeof(host)));
    assert(!native_ui_host_normalize("[desktop.example]", host, sizeof(host)));
    assert(!native_ui_host_normalize("[]", host, sizeof(host)));
}

static void test_capacity_and_in_place_normalization(void) {
    char exact[11];
    assert(native_ui_host_normalize("192.0.2.10", exact, sizeof(exact)));
    assert(strcmp(exact, "192.0.2.10") == 0);
    assert(!native_ui_host_normalize("192.0.2.10", exact, sizeof(exact) - 1u));

    char in_place[] = "[2001:db8::1]";
    assert(native_ui_host_normalize(in_place, in_place, sizeof(in_place)));
    assert(strcmp(in_place, "2001:db8::1") == 0);

    char max_input[256];
    char max_output[256];
    memset(max_input, 'a', sizeof(max_input) - 1u);
    max_input[sizeof(max_input) - 1u] = '\0';
    assert(native_ui_host_normalize(max_input, max_output, sizeof(max_output)));
    assert(strcmp(max_input, max_output) == 0);
    assert(!native_ui_host_normalize(max_input, max_output, sizeof(max_output) - 1u));
}

int main(void) {
    test_bare_hosts_are_preserved();
    test_bracketed_ipv6_is_unwrapped();
    test_malformed_brackets_are_rejected();
    test_capacity_and_in_place_normalization();
    return 0;
}
