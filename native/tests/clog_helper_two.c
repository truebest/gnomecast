/* SPDX-License-Identifier: MIT */

#include "clog.h"

clog_define(g_native_log_config, cLogLevelTrace,
            cLogFlags_PrintLevel | cLogFlags_PrintPrefix, "helper.two", NULL);

void clog_test_emit_from_helper_two(void) {
    clog(cLogLevelNotice, "two");
}
