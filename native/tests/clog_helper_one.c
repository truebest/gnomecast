/* SPDX-License-Identifier: MIT */

#include "clog.h"

clog_define(g_native_log_config, cLogLevelTrace, cLogFlags_Default, "helper.one", NULL);

void clog_test_emit_from_helper_one(void) {
    clog(cLogLevelNotice, "one");
}
