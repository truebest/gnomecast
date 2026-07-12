/* SPDX-License-Identifier: MIT */

#include <stdarg.h>
#include <stdio.h>

#include "clog.h"

static int g_evaluations;
static int g_compiled_level_evaluations;

clog_define(g_native_log_config, cLogLevelTrace, cLogFlags_Default, "NdebugTest", NULL);

bool evaluated_expression(void) {
    g_evaluations++;
    return false;
}

int evaluated_debug_argument(void) {
    g_compiled_level_evaluations++;
    return 42;
}

static void compiled_out_vlog(const char *format, ...) {
    va_list args;
    va_start(args, format);
    clog_vlogf_at(cLogLevelDebug, __FILE__, __LINE__, __func__, format, args);
    va_end(args);
}

int main(void) {
    clog_assert(evaluated_expression());
    clog_assert_msg(evaluated_expression(), "must not run");
    clog(cLogLevelDebug, "compiled out %d", evaluated_debug_argument());
    if (clog_is_enabled(cLogLevelDebug)) {
        fprintf(stderr, "compiled-out DEBUG level reported enabled\n");
        return 1;
    }
    compiled_out_vlog("compiled out");
    if (g_evaluations != 0) {
        fprintf(stderr, "NDEBUG assert evaluated its expression %d time(s)\n", g_evaluations);
        return 1;
    }
    if (g_compiled_level_evaluations != 0) {
        fprintf(stderr, "compiled minimum level evaluated a DEBUG argument\n");
        return 1;
    }
    puts("test_category_log_ndebug: all tests passed");
    return 0;
}
