/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <stdlib.h>

#include "clog.h"
#include "media_ndl_internal.h"

#define CHECK(condition_)                                                                       \
    do {                                                                                        \
        if (!(condition_)) {                                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition_); \
            exit(1);                                                                            \
        }                                                                                       \
    } while (0)

static void expect_minimum(const char *spec, BackendNdlLogLevel expected) {
    CHECK(clog_configure(spec) == cLogConfigOK);
    CHECK(native_media_ndl_test_min_level() == expected);
}

int main(void) {
    expect_minimum("", BACKEND_NDL_LOG_INFO);
    expect_minimum("media.ndl=trace", BACKEND_NDL_LOG_DEBUG);
    expect_minimum("media.ndl=debug", BACKEND_NDL_LOG_DEBUG);
    expect_minimum("media.ndl=info", BACKEND_NDL_LOG_INFO);
    expect_minimum("media.ndl=notice", BACKEND_NDL_LOG_WARN);
    expect_minimum("media.ndl=warn", BACKEND_NDL_LOG_WARN);
    expect_minimum("media.ndl=error", BACKEND_NDL_LOG_ERROR);
    expect_minimum("media.ndl=fatal", BACKEND_NDL_LOG_OFF);
    expect_minimum("media.ndl=off", BACKEND_NDL_LOG_OFF);
    return 0;
}
