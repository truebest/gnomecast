/* SPDX-License-Identifier: MIT */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "clog.h"

#define CAPTURE_MAX_EVENTS 8192u
#define CAPTURE_MESSAGE_CAPACITY 2200u

#define CHECK(condition_)                                                                                   \
    do {                                                                                                    \
        if (!(condition_)) {                                                                                \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #condition_);             \
            exit(1);                                                                                        \
        }                                                                                                   \
    } while (0)

typedef struct CapturedEvent {
    ClogxLevel level;
    ClogxFlags flags;
    char category[80];
    char message[CAPTURE_MESSAGE_CAPACITY];
    char file[160];
    char function[80];
    int line;
    uint64_t wall_time_ns;
    uint64_t monotonic_time_ns;
    bool truncated;
} CapturedEvent;

typedef struct Capture {
    pthread_mutex_t lock;
    size_t count;
    CapturedEvent events[CAPTURE_MAX_EVENTS];
} Capture;

typedef struct BlockingSink {
    pthread_mutex_t lock;
    pthread_cond_t condition;
    bool entered;
    bool release;
} BlockingSink;

typedef struct SinkSetter {
    atomic_bool started;
    atomic_bool returned;
} SinkSetter;

static Capture g_capture = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

void clog_test_emit_from_helper_one(void);
void clog_test_emit_from_helper_two(void);

CLOGX_CATEGORY_DEFINE(g_test_core, "test.core", CLOGX_LEVEL_INFO);
CLOGX_CATEGORY_DEFINE(g_test_audio, "audio", CLOGX_LEVEL_INFO);
CLOGX_CATEGORY_DEFINE(g_test_audio_pipeline, "audio.pipeline", CLOGX_LEVEL_INFO);
CLOGX_CATEGORY_DEFINE(g_test_audio_other, "audio.other", CLOGX_LEVEL_NOTICE);
CLOGX_CATEGORY_DEFINE(g_test_audiox, "audiox", CLOGX_LEVEL_INFO);
CLOGX_CATEGORY_DEFINE(g_test_late, "late.child", CLOGX_LEVEL_INFO);
CLOGX_CATEGORY_DEFINE(g_test_format, "format", CLOGX_LEVEL_TRACE);
CLOGX_CATEGORY_DEFINE(g_test_rate, "rate", CLOGX_LEVEL_TRACE);
CLOGX_CATEGORY_DEFINE(g_test_threads, "threads", CLOGX_LEVEL_INFO);

/* This is the primary API expected in application translation units. */
clog_define(g_native_log_config, cLogLevelTrace, cLogFlags_Default, "ExampleApp", NULL);

static uint64_t g_fake_wall_ns;
static uint64_t g_fake_monotonic_ns;
static int g_level_evaluations;

static uint64_t fake_wall_clock(void) {
    return g_fake_wall_ns;
}

static uint64_t fake_monotonic_clock(void) {
    return g_fake_monotonic_ns;
}

static cLogLevel evaluated_level(void) {
    g_level_evaluations++;
    return cLogLevelError;
}

static void facade_vlog(cLogLevel level, const char *format, ...) {
    va_list args;
    va_start(args, format);
    clog_vlogf_at(level, "upstream.c", 17, "upstream_log", format, args);
    va_end(args);
}

static void emit_facade_limited(unsigned value) {
    clog_limited(cLogLevelNotice, 2, 100, "facade limited %u", value);
}

static void copy_string(char *dest, size_t capacity, const char *source) {
    if (!source) {
        source = "";
    }
    (void)snprintf(dest, capacity, "%s", source);
}

static void capture_sink(const ClogxEvent *event, void *context) {
    Capture *capture = (Capture *)context;
    CHECK(event);
    CHECK(capture);
    pthread_mutex_lock(&capture->lock);
    size_t index = capture->count++;
    if (index < CAPTURE_MAX_EVENTS) {
        CapturedEvent *stored = &capture->events[index];
        memset(stored, 0, sizeof(*stored));
        stored->level = event->level;
        stored->flags = event->flags;
        copy_string(stored->category, sizeof(stored->category), event->category);
        copy_string(stored->message, sizeof(stored->message), event->message);
        copy_string(stored->file, sizeof(stored->file), event->file);
        copy_string(stored->function, sizeof(stored->function), event->function);
        stored->line = event->line;
        stored->wall_time_ns = event->wall_time_ns;
        stored->monotonic_time_ns = event->monotonic_time_ns;
        stored->truncated = event->message_truncated;
    }
    pthread_mutex_unlock(&capture->lock);
}

static void blocking_sink(const ClogxEvent *event, void *context) {
    BlockingSink *blocking = (BlockingSink *)context;
    CHECK(event != NULL);
    CHECK(blocking != NULL);
    pthread_mutex_lock(&blocking->lock);
    blocking->entered = true;
    pthread_cond_broadcast(&blocking->condition);
    while (!blocking->release) {
        pthread_cond_wait(&blocking->condition, &blocking->lock);
    }
    pthread_mutex_unlock(&blocking->lock);
}

static void capture_reset(void) {
    pthread_mutex_lock(&g_capture.lock);
    g_capture.count = 0;
    memset(g_capture.events, 0, sizeof(g_capture.events));
    pthread_mutex_unlock(&g_capture.lock);
}

static size_t capture_count(void) {
    pthread_mutex_lock(&g_capture.lock);
    size_t count = g_capture.count;
    pthread_mutex_unlock(&g_capture.lock);
    return count;
}

static CapturedEvent capture_event(size_t index) {
    pthread_mutex_lock(&g_capture.lock);
    CHECK(index < g_capture.count);
    CHECK(index < CAPTURE_MAX_EVENTS);
    CapturedEvent event = g_capture.events[index];
    pthread_mutex_unlock(&g_capture.lock);
    return event;
}

static void reset_logger(void) {
    CHECK(clogx_configure("") == CLOGX_CONFIG_OK);
    g_fake_wall_ns = UINT64_C(1700000000123000000);
    g_fake_monotonic_ns = UINT64_C(5000000000);
    clogx_test_set_clocks(fake_wall_clock, fake_monotonic_clock);
    clogx_set_sink(capture_sink, &g_capture);
    capture_reset();
}

static void test_levels_and_disabled_arguments(void) {
    reset_logger();
    CHECK(strcmp(clogx_level_name(CLOGX_LEVEL_TRACE), "TRACE") == 0);
    CHECK(strcmp(clogx_level_name(CLOGX_LEVEL_FATAL), "FATAL") == 0);
    CHECK(strcmp(clogx_level_name((ClogxLevel)99), "UNKNOWN") == 0);
    CHECK(!clogx_enabled(NULL, CLOGX_LEVEL_INFO));
    CHECK(!clogx_enabled(&g_test_core, CLOGX_LEVEL_OFF));
    CHECK(!clogx_enabled(&g_test_core, (ClogxLevel)-1));
    CHECK(!clogx_enabled(&g_test_core, CLOGX_LEVEL_DEBUG));
    CHECK(clogx_enabled(&g_test_core, CLOGX_LEVEL_INFO));

    int side_effect = 0;
    CLOGX_DEBUG(&g_test_core, "disabled %d", ++side_effect);
    CHECK(side_effect == 0);
    CHECK(capture_count() == 0);

    CLOGX_INFO(&g_test_core, "hello %d", 7);
    CHECK(capture_count() == 1);
    CapturedEvent event = capture_event(0);
    CHECK(event.level == CLOGX_LEVEL_INFO);
    CHECK(strcmp(event.category, "test.core") == 0);
    CHECK(strcmp(event.message, "hello 7") == 0);
    CHECK(event.file[0] != '\0' && event.function[0] != '\0' && event.line > 0);
    CHECK(event.wall_time_ns == g_fake_wall_ns);
    CHECK(event.monotonic_time_ns == 0);
}

static void test_lowercase_facade(void) {
    reset_logger();
    CHECK(strcmp(g_native_log_config.name, "ExampleApp") == 0);
    CHECK(g_native_log_config.default_level == cLogLevelTrace);
    CHECK(g_native_log_config.flags == cLogFlags_Default);
    CHECK(g_native_log_config.config == NULL);

    clog(cLogLevelNotice, "Hello, World!\n");
    clog(cLogLevelError, "Error: %d\n", 7);
    CHECK(capture_count() == 2);
    CapturedEvent notice = capture_event(0);
    CapturedEvent error = capture_event(1);
    CHECK(notice.level == cLogLevelNotice);
    CHECK(notice.flags == cLogFlags_Default);
    CHECK(strcmp(notice.category, "ExampleApp") == 0);
    CHECK(strcmp(notice.message, "Hello, World!") == 0);
    CHECK(error.level == cLogLevelError);
    CHECK(strcmp(error.message, "Error: 7") == 0);

    CHECK(clog_configure("ExampleApp=error") == cLogConfigOK);
    int side_effect = 0;
    clog(cLogLevelNotice, "disabled %d", ++side_effect);
    CHECK(side_effect == 0);
    CHECK(capture_count() == 2);
    CHECK(clog_is_enabled(cLogLevelError));
    CHECK(!clog_is_enabled(cLogLevelNotice));

    g_level_evaluations = 0;
    clog(evaluated_level(), "dynamic level");
    CHECK(g_level_evaluations == 1);
    CHECK(capture_count() == 3);

    g_level_evaluations = 0;
    CHECK(clog_is_enabled(evaluated_level()));
    CHECK(g_level_evaluations == 1);
}

static void test_file_local_definitions(void) {
    reset_logger();
    clog_test_emit_from_helper_one();
    clog_test_emit_from_helper_two();
    CHECK(capture_count() == 2);
    CHECK(strcmp(capture_event(0).category, "helper.one") == 0);
    CapturedEvent helper_two = capture_event(1);
    CHECK(strcmp(helper_two.category, "helper.two") == 0);
    CHECK(helper_two.flags == (cLogFlags_PrintLevel | cLogFlags_PrintPrefix));
}

static void test_facade_vlog_and_rate_limit(void) {
    reset_logger();
    facade_vlog(cLogLevelNotice, "forwarded %d", 9);
    CHECK(capture_count() == 1);
    CapturedEvent forwarded = capture_event(0);
    CHECK(strcmp(forwarded.category, "ExampleApp") == 0);
    CHECK(strcmp(forwarded.message, "forwarded 9") == 0);
    CHECK(strcmp(forwarded.file, "upstream.c") == 0);
    CHECK(forwarded.line == 17);

    g_fake_monotonic_ns = UINT64_C(1000000000);
    emit_facade_limited(1);
    emit_facade_limited(2);
    emit_facade_limited(3);
    CHECK(capture_count() == 3);
    g_fake_monotonic_ns += UINT64_C(101000000);
    emit_facade_limited(4);
    CHECK(capture_count() == 5);
    CHECK(strcmp(capture_event(3).message, "suppressed 1 similar messages") == 0);
    CHECK(strcmp(capture_event(4).message, "facade limited 4") == 0);
}

static void test_rules_and_transactionality(void) {
    reset_logger();
    CHECK(clogx_configure("*=warn,audio=debug,audio.pipeline=error") == CLOGX_CONFIG_OK);
    CHECK(clogx_enabled(&g_test_audio, CLOGX_LEVEL_DEBUG));
    CHECK(!clogx_enabled(&g_test_audio_pipeline, CLOGX_LEVEL_WARN));
    CHECK(clogx_enabled(&g_test_audio_pipeline, CLOGX_LEVEL_ERROR));
    CHECK(clogx_enabled(&g_test_audio_other, CLOGX_LEVEL_DEBUG));
    CHECK(!clogx_enabled(&g_test_audiox, CLOGX_LEVEL_INFO));
    CHECK(clogx_enabled(&g_test_audiox, CLOGX_LEVEL_WARN));

    CHECK(clogx_configure(" audio = info , audio.pipeline = trace ") == CLOGX_CONFIG_OK);
    CHECK(clogx_enabled(&g_test_audio_pipeline, CLOGX_LEVEL_TRACE));
    CHECK(clogx_configure("audio=DEBUG") == CLOGX_CONFIG_INVALID);
    CHECK(clogx_enabled(&g_test_audio_pipeline, CLOGX_LEVEL_TRACE));
    CHECK(clogx_configure("audio=debug,") == CLOGX_CONFIG_INVALID);
    CHECK(clogx_configure("audio..pipeline=info") == CLOGX_CONFIG_INVALID);
    CHECK(clogx_configure("audio:level=info") == CLOGX_CONFIG_INVALID);
    CHECK(clogx_configure(NULL) == CLOGX_CONFIG_INVALID);

    char too_many[512] = "";
    for (unsigned i = 0; i < 32; i++) {
        (void)strcat(too_many, i == 0 ? "a=info" : ",a=info");
    }
    CHECK(clogx_configure(too_many) == CLOGX_CONFIG_OK);
    (void)strcat(too_many, ",a=info");
    CHECK(clogx_configure(too_many) == CLOGX_CONFIG_TOO_MANY_RULES);

    char selector[65];
    memset(selector, 'a', 63);
    selector[63] = '\0';
    char selector_rule[80];
    (void)snprintf(selector_rule, sizeof(selector_rule), "%s=trace", selector);
    CHECK(clogx_configure(selector_rule) == CLOGX_CONFIG_OK);
    selector[63] = 'a';
    selector[64] = '\0';
    (void)snprintf(selector_rule, sizeof(selector_rule), "%s=trace", selector);
    CHECK(clogx_configure(selector_rule) == CLOGX_CONFIG_INVALID);

    CHECK(clogx_configure("") == CLOGX_CONFIG_OK);
    CHECK(!clogx_enabled(&g_test_audio_pipeline, CLOGX_LEVEL_DEBUG));
    CHECK(clogx_enabled(&g_test_audio_pipeline, CLOGX_LEVEL_INFO));
}

static void test_rules_apply_to_late_categories(void) {
    reset_logger();
    CHECK(!atomic_load(&g_test_late.registered));
    CHECK(clogx_configure("late=trace") == CLOGX_CONFIG_OK);
    CHECK(clogx_enabled(&g_test_late, CLOGX_LEVEL_TRACE));
    CHECK(atomic_load(&g_test_late.registered));
}

static void test_environment_configuration(void) {
    reset_logger();
    CHECK(setenv("GNOMECAST_LOG", "test.core=debug", 1) == 0);
    CHECK(clogx_configure_env() == CLOGX_CONFIG_OK);
    CHECK(clogx_enabled(&g_test_core, CLOGX_LEVEL_DEBUG));

    capture_reset();
    CHECK(setenv("GNOMECAST_LOG", "test.core=loud", 1) == 0);
    CHECK(clogx_configure_env() == CLOGX_CONFIG_INVALID);
    CHECK(clogx_enabled(&g_test_core, CLOGX_LEVEL_DEBUG));
    CHECK(capture_count() == 1);
    CapturedEvent warning = capture_event(0);
    CHECK(strcmp(warning.category, "diagnostics.internal") == 0);
    CHECK(warning.level == CLOGX_LEVEL_WARN);

    CHECK(unsetenv("GNOMECAST_LOG") == 0);
    CHECK(clogx_configure_env() == CLOGX_CONFIG_OK);
    CHECK(!clogx_enabled(&g_test_core, CLOGX_LEVEL_DEBUG));
}

static void test_message_sanitizing_and_truncation(void) {
    reset_logger();
    CLOGX_INFO(&g_test_core, "line one\nline two\r\n");
    CapturedEvent multiline = capture_event(0);
    CHECK(strcmp(multiline.message, "line one line two") == 0);
    CHECK(!multiline.truncated);

    char huge[4096];
    memset(huge, 'x', sizeof(huge) - 1u);
    huge[sizeof(huge) - 1u] = '\0';
    CLOGX_INFO(&g_test_core, "%s", huge);
    CapturedEvent truncated = capture_event(1);
    CHECK(truncated.truncated);
    CHECK(strlen(truncated.message) < 2048u);
    CHECK(strstr(truncated.message, "<truncated>") != NULL);
}

static void test_default_formatter(void) {
    ClogxEvent event = {
        .level = CLOGX_LEVEL_INFO,
        .flags = CLOGX_FLAGS_DEFAULT,
        .category = "format",
        .message = "hello",
        .file = "test.c",
        .function = "test_fn",
        .line = 42,
        .wall_time_ns = UINT64_C(1700000000123000000),
        .monotonic_time_ns = UINT64_C(123456000000),
    };
    char line[512];
    size_t required = clogx_test_format_event(&event, line, sizeof(line));
    CHECK(required == strlen(line));
    CHECK(strstr(line, "+000123.456 INFO format: hello\n") != NULL);
    CHECK(strstr(line, "test.c") == NULL);

    event.level = CLOGX_LEVEL_DEBUG;
    required = clogx_test_format_event(&event, line, sizeof(line));
    CHECK(required == strlen(line));
    CHECK(strstr(line, "DEBUG format: test.c:42 test_fn: hello\n") != NULL);

    size_t size_only = clogx_test_format_event(&event, NULL, 0);
    CHECK(size_only == required);
    char tiny[24];
    CHECK(clogx_test_format_event(&event, tiny, sizeof(tiny)) == required);
    CHECK(tiny[sizeof(tiny) - 1u] == '\0');
    CHECK(strstr(tiny, "<truncated>") != NULL);

    event.level = CLOGX_LEVEL_INFO;
    event.flags = CLOGX_FLAG_NONE;
    CHECK(clogx_test_format_event(&event, line, sizeof(line)) == strlen("hello\n"));
    CHECK(strcmp(line, "hello\n") == 0);

    event.level = CLOGX_LEVEL_DEBUG;
    CHECK(clogx_test_format_event(&event, line, sizeof(line)) == strlen("hello\n"));
    CHECK(strcmp(line, "hello\n") == 0);

    event.level = CLOGX_LEVEL_INFO;
    event.flags = CLOGX_FLAG_PRINT_LEVEL | CLOGX_FLAG_PRINT_CATEGORY;
    CHECK(clogx_test_format_event(&event, line, sizeof(line)) == strlen("INFO format: hello\n"));
    CHECK(strcmp(line, "INFO format: hello\n") == 0);

    event.flags = CLOGX_FLAG_PRINT_SOURCE;
    CHECK(clogx_test_format_event(&event, line, sizeof(line)) == strlen("test.c:42 test_fn: hello\n"));
    CHECK(strcmp(line, "test.c:42 test_fn: hello\n") == 0);
}

static void emit_rate_limited(unsigned value) {
    CLOGX_LOG_LIMITED(&g_test_rate, CLOGX_LEVEL_INFO, 2, 100, "limited %u", value);
}

static void test_rate_limiter(void) {
    reset_logger();
    g_fake_monotonic_ns = UINT64_C(1000000000);
    emit_rate_limited(1);
    emit_rate_limited(2);
    emit_rate_limited(3);
    CHECK(capture_count() == 2);
    g_fake_monotonic_ns += UINT64_C(100000000);
    emit_rate_limited(4);
    CHECK(capture_count() == 4);
    CHECK(strcmp(capture_event(2).message, "suppressed 1 similar messages") == 0);
    CHECK(strcmp(capture_event(3).message, "limited 4") == 0);
    CHECK(clogx_rate_limit_allow(NULL, 1, 1, NULL));

    ClogxRateLimit direct = CLOGX_RATE_LIMIT_INITIALIZER;
    uint32_t suppressed = 0;
    g_fake_monotonic_ns = UINT64_C(2000000000);
    CHECK(clogx_rate_limit_allow(&direct, 1, UINT64_MAX, &suppressed));
    CHECK(!clogx_rate_limit_allow(&direct, 1, UINT64_MAX, &suppressed));
    g_fake_monotonic_ns = UINT64_C(1000000000);
    CHECK(clogx_rate_limit_allow(&direct, 1, UINT64_MAX, &suppressed));
    CHECK(suppressed == 1);
}

static void test_assert_and_panic(void) {
    reset_logger();
    CHECK(clogx_configure("*=trace,diagnostics.backtrace=off,diagnostics.break=off") == CLOGX_CONFIG_OK);
    CLOGX_ASSERT(&g_test_core, 1 == 2);
    CLOGX_ASSERT_MSG(&g_test_audio, false, "bad value %d", 9);
    CHECK(capture_count() == 2);
    CHECK(strcmp(capture_event(0).category, "diagnostics.assert") == 0);
    CHECK(strstr(capture_event(0).message, "test.core: assertion failed: 1 == 2") != NULL);
    CHECK(strstr(capture_event(1).message, "audio: assertion failed: false (bad value 9)") != NULL);

    char huge[4096];
    memset(huge, 'a', sizeof(huge) - 1u);
    huge[sizeof(huge) - 1u] = '\0';
    CLOGX_ASSERT_MSG(&g_test_core, false, "%s", huge);
    CapturedEvent long_assert = capture_event(2);
    CHECK(long_assert.truncated);
    CHECK(strstr(long_assert.message, "<truncated>") != NULL);

    capture_reset();
    CHECK(clogx_configure("*=trace,diagnostics.backtrace=error,diagnostics.break=off") == CLOGX_CONFIG_OK);
    CLOGX_ASSERT(&g_test_core, false);
    CHECK(capture_count() >= 1);
    CHECK(strcmp(capture_event(0).category, "diagnostics.assert") == 0);
#if defined(CLOGX_HAVE_EXECINFO) && CLOGX_HAVE_EXECINFO
    CHECK(capture_count() > 1);
    CHECK(strcmp(capture_event(1).category, "diagnostics.backtrace") == 0);
#endif

    CHECK(clogx_configure("diagnostics.break=fatal") == CLOGX_CONFIG_OK);
    CHECK(!clogx_test_break_enabled(CLOGX_LEVEL_ERROR));
    CHECK(clogx_test_break_enabled(CLOGX_LEVEL_FATAL));

    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        (void)clogx_configure("*=off");
        CLOGX_PANIC(&g_test_core, "forced panic");
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGABRT);
}

static void *single_log_thread(void *arg) {
    (void)arg;
    CLOGX_INFO(&g_test_threads, "blocking sink event");
    return NULL;
}

static void *sink_setter_thread(void *arg) {
    SinkSetter *setter = (SinkSetter *)arg;
    atomic_store(&setter->started, true);
    clogx_set_sink(capture_sink, &g_capture);
    atomic_store(&setter->returned, true);
    return NULL;
}

static void test_sink_replacement_drains_callbacks(void) {
    reset_logger();
    BlockingSink blocking = {
        .lock = PTHREAD_MUTEX_INITIALIZER,
        .condition = PTHREAD_COND_INITIALIZER,
    };
    SinkSetter setter;
    atomic_init(&setter.started, false);
    atomic_init(&setter.returned, false);
    clogx_set_sink(blocking_sink, &blocking);

    pthread_t logger_thread;
    CHECK(pthread_create(&logger_thread, NULL, single_log_thread, NULL) == 0);
    pthread_mutex_lock(&blocking.lock);
    while (!blocking.entered) {
        pthread_cond_wait(&blocking.condition, &blocking.lock);
    }
    pthread_mutex_unlock(&blocking.lock);

    pthread_t setter_thread;
    CHECK(pthread_create(&setter_thread, NULL, sink_setter_thread, &setter) == 0);
    while (!atomic_load(&setter.started)) {
    }
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 20 * 1000 * 1000};
    (void)nanosleep(&pause, NULL);
    CHECK(!atomic_load(&setter.returned));

    pthread_mutex_lock(&blocking.lock);
    blocking.release = true;
    pthread_cond_broadcast(&blocking.condition);
    pthread_mutex_unlock(&blocking.lock);
    CHECK(pthread_join(logger_thread, NULL) == 0);
    CHECK(pthread_join(setter_thread, NULL) == 0);
    CHECK(atomic_load(&setter.returned));
    pthread_cond_destroy(&blocking.condition);
    pthread_mutex_destroy(&blocking.lock);
}

static void emit_concurrent_limited(void) {
    CLOGX_LOG_LIMITED(&g_test_rate, CLOGX_LEVEL_INFO, 10, 1000, "concurrent limited");
}

static void *rate_thread(void *arg) {
    (void)arg;
    for (unsigned i = 0; i < 100; i++) {
        emit_concurrent_limited();
    }
    return NULL;
}

static void test_concurrent_rate_limiter(void) {
    reset_logger();
    g_fake_monotonic_ns = UINT64_C(3000000000);
    pthread_t threads[4];
    for (size_t i = 0; i < sizeof(threads) / sizeof(threads[0]); i++) {
        CHECK(pthread_create(&threads[i], NULL, rate_thread, NULL) == 0);
    }
    for (size_t i = 0; i < sizeof(threads) / sizeof(threads[0]); i++) {
        CHECK(pthread_join(threads[i], NULL) == 0);
    }
    CHECK(capture_count() == 10);
    g_fake_monotonic_ns += UINT64_C(1000000000);
    emit_concurrent_limited();
    CHECK(capture_count() == 12);
    CHECK(strcmp(capture_event(10).message, "suppressed 390 similar messages") == 0);
}

static void *logging_thread(void *arg) {
    uintptr_t id = (uintptr_t)arg;
    for (unsigned i = 0; i < 500; i++) {
        CLOGX_INFO(&g_test_threads, "worker %lu event %u", (unsigned long)id, i);
    }
    return NULL;
}

static void test_concurrent_logging_and_configuration(void) {
    reset_logger();
    pthread_t threads[4];
    for (uintptr_t i = 0; i < 4; i++) {
        CHECK(pthread_create(&threads[i], NULL, logging_thread, (void *)i) == 0);
    }
    for (unsigned i = 0; i < 500; i++) {
        CHECK(clogx_configure((i & 1u) ? "threads=off" : "threads=trace") == CLOGX_CONFIG_OK);
    }
    for (size_t i = 0; i < sizeof(threads) / sizeof(threads[0]); i++) {
        CHECK(pthread_join(threads[i], NULL) == 0);
    }
    CHECK(capture_count() <= 2000);
    CHECK(clogx_configure("") == CLOGX_CONFIG_OK);
}

int main(void) {
    test_levels_and_disabled_arguments();
    test_lowercase_facade();
    test_file_local_definitions();
    test_facade_vlog_and_rate_limit();
    test_rules_and_transactionality();
    test_rules_apply_to_late_categories();
    test_environment_configuration();
    test_message_sanitizing_and_truncation();
    test_default_formatter();
    test_rate_limiter();
    test_assert_and_panic();
    test_sink_replacement_drains_callbacks();
    test_concurrent_rate_limiter();
    test_concurrent_logging_and_configuration();
    clogx_reset_sink();
    clogx_test_set_clocks(NULL, NULL);
    puts("test_category_log: all tests passed");
    return 0;
}
