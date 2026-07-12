/* SPDX-License-Identifier: MIT */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "category_log.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__unix__) || defined(__APPLE__)
#include <sched.h>
#endif

#if defined(__linux__)
#include <signal.h>
#endif

#if defined(CLOGX_HAVE_EXECINFO) && CLOGX_HAVE_EXECINFO
#include <execinfo.h>
#endif

#define CLOGX_MAX_RULES 32u
#define CLOGX_MAX_SELECTOR 63u
#define CLOGX_MESSAGE_CAPACITY 2048u
#define CLOGX_LINE_CAPACITY 2048u
#define CLOGX_BACKTRACE_FRAMES 32
#define CLOGX_NS_PER_SECOND UINT64_C(1000000000)
#define CLOGX_NS_PER_MILLISECOND UINT64_C(1000000)

typedef struct ClogxRule {
    char selector[CLOGX_MAX_SELECTOR + 1u];
    ClogxLevel level;
} ClogxRule;

typedef struct ClogxSinkSlot {
    ClogxSink sink;
    void *context;
    uint32_t in_flight;
} ClogxSinkSlot;

static atomic_flag g_state_lock = ATOMIC_FLAG_INIT;
static ClogxCategory *g_categories;
static ClogxRule g_rules[CLOGX_MAX_RULES];
static size_t g_rule_count;
static ClogxSinkSlot g_sink_slots[2];
static unsigned g_active_sink_slot;
static atomic_flag g_sink_set_lock = ATOMIC_FLAG_INIT;

static atomic_flag g_clock_lock = ATOMIC_FLAG_INIT;
static uint64_t g_monotonic_origin_ns;
static uint64_t g_monotonic_fallback_last_ns;

#ifdef CLOGX_TESTING
static ClogxTestClock g_test_wall_clock;
static ClogxTestClock g_test_monotonic_clock;
#endif

static ClogxCategory g_diagnostics_assert =
    CLOGX_CATEGORY_INITIALIZER("diagnostics.assert", CLOGX_LEVEL_ERROR);
static ClogxCategory g_diagnostics_backtrace =
    CLOGX_CATEGORY_INITIALIZER("diagnostics.backtrace", CLOGX_LEVEL_ERROR);
static ClogxCategory g_diagnostics_break =
    CLOGX_CATEGORY_INITIALIZER("diagnostics.break", CLOGX_LEVEL_OFF);
static ClogxCategory g_diagnostics_internal =
    CLOGX_CATEGORY_INITIALIZER("diagnostics.internal", CLOGX_LEVEL_WARN);

static void clogx_lock(atomic_flag *lock) {
    while (atomic_flag_test_and_set_explicit(lock, memory_order_acquire)) {
    }
}

static void clogx_unlock(atomic_flag *lock) {
    atomic_flag_clear_explicit(lock, memory_order_release);
}

static void clogx_thread_yield(void) {
#if defined(__unix__) || defined(__APPLE__)
    (void)sched_yield();
#endif
}

static uint64_t clogx_timespec_ns(const struct timespec *ts) {
    if (!ts || ts->tv_sec < 0 || ts->tv_nsec < 0) {
        return 0;
    }
    return (uint64_t)ts->tv_sec * CLOGX_NS_PER_SECOND + (uint64_t)ts->tv_nsec;
}

static uint64_t clogx_wall_now_ns(void) {
#ifdef CLOGX_TESTING
    if (g_test_wall_clock) {
        return g_test_wall_clock();
    }
#endif
    struct timespec ts;
#if defined(CLOGX_HAVE_CLOCK_GETTIME) && CLOGX_HAVE_CLOCK_GETTIME
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        return clogx_timespec_ns(&ts);
    }
#endif
#if defined(TIME_UTC)
    if (timespec_get(&ts, TIME_UTC) == TIME_UTC) {
        return clogx_timespec_ns(&ts);
    }
#endif
    return (uint64_t)time(NULL) * CLOGX_NS_PER_SECOND;
}

static uint64_t clogx_monotonic_raw_ns(void) {
#ifdef CLOGX_TESTING
    if (g_test_monotonic_clock) {
        return g_test_monotonic_clock();
    }
#endif
#if defined(CLOGX_HAVE_CLOCK_GETTIME) && CLOGX_HAVE_CLOCK_GETTIME
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return clogx_timespec_ns(&ts);
    }
#endif
    /* ISO C has no steady clock. Unlike clock(), this fallback advances while the
     * process sleeps or blocks. Clamp backward wall-clock corrections so callers still
     * observe a nondecreasing value; forward clock corrections may advance it early. */
    uint64_t now = clogx_wall_now_ns();
    clogx_lock(&g_clock_lock);
    if (now < g_monotonic_fallback_last_ns) {
        now = g_monotonic_fallback_last_ns;
    } else {
        g_monotonic_fallback_last_ns = now;
    }
    clogx_unlock(&g_clock_lock);
    return now;
}

static uint64_t clogx_monotonic_elapsed_ns(void) {
    uint64_t now = clogx_monotonic_raw_ns();
    clogx_lock(&g_clock_lock);
    if (g_monotonic_origin_ns == 0) {
        g_monotonic_origin_ns = now ? now : 1;
    }
    uint64_t origin = g_monotonic_origin_ns;
    clogx_unlock(&g_clock_lock);
    return now >= origin ? now - origin : 0;
}

const char *clogx_level_name(ClogxLevel level) {
    static const char *const names[] = {
        "TRACE", "DEBUG", "INFO", "NOTICE", "WARN", "ERROR", "FATAL", "OFF",
    };
    return (unsigned)level < sizeof(names) / sizeof(names[0]) ? names[level] : "UNKNOWN";
}

static bool clogx_selector_matches(const char *selector, const char *category) {
    if (strcmp(selector, "*") == 0) {
        return true;
    }
    size_t selector_len = strlen(selector);
    size_t category_len = strlen(category);
    if (category_len < selector_len) {
        return false;
    }
    return strncmp(selector, category, selector_len) == 0 &&
           (category[selector_len] == '\0' || category[selector_len] == '.');
}

static ClogxLevel clogx_effective_level_locked(const ClogxCategory *category) {
    ClogxLevel level = category->default_level;
    for (size_t i = 0; i < g_rule_count; i++) {
        if (clogx_selector_matches(g_rules[i].selector, category->name)) {
            level = g_rules[i].level;
        }
    }
    return level;
}

static void clogx_register_category(ClogxCategory *category) {
    if (atomic_load_explicit(&category->registered, memory_order_acquire)) {
        return;
    }
    clogx_lock(&g_state_lock);
    if (!atomic_load_explicit(&category->registered, memory_order_relaxed)) {
        atomic_store_explicit(&category->effective_level, clogx_effective_level_locked(category),
                              memory_order_relaxed);
        category->next = g_categories;
        g_categories = category;
        atomic_store_explicit(&category->registered, true, memory_order_release);
    }
    clogx_unlock(&g_state_lock);
}

bool clogx_enabled(ClogxCategory *category, ClogxLevel level) {
    if (!category || !category->name || level < CLOGX_LEVEL_TRACE || level >= CLOGX_LEVEL_OFF) {
        return false;
    }
    clogx_register_category(category);
    ClogxLevel threshold = (ClogxLevel)atomic_load_explicit(&category->effective_level, memory_order_relaxed);
    return level >= threshold && threshold < CLOGX_LEVEL_OFF;
}

bool clogx_enabled_at_or_above(ClogxCategory *category, ClogxLevel level, int compiled_min_level) {
    return (int)level >= compiled_min_level && clogx_enabled(category, level);
}

static bool clogx_valid_selector(const char *selector) {
    if (!selector || !selector[0]) {
        return false;
    }
    if (strcmp(selector, "*") == 0) {
        return true;
    }
    bool previous_dot = true;
    for (const unsigned char *p = (const unsigned char *)selector; *p; p++) {
        if (*p == '.') {
            if (previous_dot) {
                return false;
            }
            previous_dot = true;
        } else if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                   (*p >= '0' && *p <= '9') || *p == '_' || *p == '-') {
            previous_dot = false;
        } else {
            return false;
        }
    }
    return !previous_dot;
}

static bool clogx_parse_level(const char *text, ClogxLevel *out_level) {
    static const struct {
        const char *name;
        ClogxLevel level;
    } levels[] = {
        {"trace", CLOGX_LEVEL_TRACE},   {"debug", CLOGX_LEVEL_DEBUG},
        {"info", CLOGX_LEVEL_INFO},     {"notice", CLOGX_LEVEL_NOTICE},
        {"warn", CLOGX_LEVEL_WARN},     {"error", CLOGX_LEVEL_ERROR},
        {"fatal", CLOGX_LEVEL_FATAL},   {"off", CLOGX_LEVEL_OFF},
    };
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        if (strcmp(text, levels[i].name) == 0) {
            *out_level = levels[i].level;
            return true;
        }
    }
    return false;
}

static char *clogx_trim(char *text) {
    while (*text && isspace((unsigned char)*text)) {
        text++;
    }
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return text;
}

int clogx_configure(const char *spec) {
    if (!spec) {
        return CLOGX_CONFIG_INVALID;
    }

    ClogxRule parsed[CLOGX_MAX_RULES];
    size_t parsed_count = 0;
    const char *cursor = spec;
    while (*cursor) {
        const char *comma = strchr(cursor, ',');
        size_t segment_len = comma ? (size_t)(comma - cursor) : strlen(cursor);
        if (segment_len == 0 || segment_len > (CLOGX_MAX_SELECTOR + 16u)) {
            return CLOGX_CONFIG_INVALID;
        }
        char segment[CLOGX_MAX_SELECTOR + 17u];
        memcpy(segment, cursor, segment_len);
        segment[segment_len] = '\0';

        char *entry = clogx_trim(segment);
        char *equals = strchr(entry, '=');
        if (!equals || strchr(equals + 1, '=')) {
            return CLOGX_CONFIG_INVALID;
        }
        *equals = '\0';
        char *selector = clogx_trim(entry);
        char *level_text = clogx_trim(equals + 1);
        size_t selector_len = strlen(selector);
        if (parsed_count >= CLOGX_MAX_RULES) {
            return CLOGX_CONFIG_TOO_MANY_RULES;
        }
        if (!clogx_valid_selector(selector) || selector_len > CLOGX_MAX_SELECTOR ||
            !clogx_parse_level(level_text, &parsed[parsed_count].level)) {
            return CLOGX_CONFIG_INVALID;
        }
        memcpy(parsed[parsed_count].selector, selector, selector_len + 1u);
        parsed_count++;

        if (!comma) {
            break;
        }
        cursor = comma + 1;
        if (!*cursor) {
            return CLOGX_CONFIG_INVALID;
        }
    }

    clogx_lock(&g_state_lock);
    memcpy(g_rules, parsed, parsed_count * sizeof(parsed[0]));
    g_rule_count = parsed_count;
    for (ClogxCategory *category = g_categories; category; category = category->next) {
        atomic_store_explicit(&category->effective_level, clogx_effective_level_locked(category),
                              memory_order_relaxed);
    }
    clogx_unlock(&g_state_lock);
    return CLOGX_CONFIG_OK;
}

int clogx_configure_env(void) {
    const char *spec = getenv("GNOMECAST_LOG");
    int result = clogx_configure(spec ? spec : "");
    if (result != CLOGX_CONFIG_OK) {
        clogx_logf(&g_diagnostics_internal, CLOGX_LEVEL_WARN, __FILE__, __LINE__, __func__,
                   "invalid GNOMECAST_LOG rules ignored (status=%d)", result);
    }
    return result;
}

void clogx_set_sink(ClogxSink sink, void *context) {
    clogx_lock(&g_sink_set_lock);

    unsigned old_slot;
    for (;;) {
        clogx_lock(&g_state_lock);
        old_slot = g_active_sink_slot;
        unsigned new_slot = 1u - old_slot;
        if (g_sink_slots[new_slot].in_flight == 0) {
            g_sink_slots[new_slot].sink = sink;
            g_sink_slots[new_slot].context = context;
            g_active_sink_slot = new_slot;
            clogx_unlock(&g_state_lock);
            break;
        }
        clogx_unlock(&g_state_lock);
        clogx_thread_yield();
    }

    for (;;) {
        clogx_lock(&g_state_lock);
        bool drained = g_sink_slots[old_slot].in_flight == 0;
        clogx_unlock(&g_state_lock);
        if (drained) {
            break;
        }
        clogx_thread_yield();
    }
    clogx_unlock(&g_sink_set_lock);
}

void clogx_reset_sink(void) {
    clogx_set_sink(NULL, NULL);
}

static size_t clogx_append(char *output, size_t capacity, size_t offset, const char *text, size_t text_len) {
    if (capacity > 0 && offset < capacity - 1u) {
        size_t available = capacity - 1u - offset;
        size_t copied = text_len < available ? text_len : available;
        memcpy(output + offset, text, copied);
        output[offset + copied] = '\0';
    }
    return offset + text_len;
}

static size_t clogx_format_event(const ClogxEvent *event, char *output, size_t capacity) {
    ClogxFlags flags = event->flags;
    char metadata[256];
    size_t metadata_len = 0;
    uint64_t wall_ms = (event->wall_time_ns / CLOGX_NS_PER_MILLISECOND) % 1000u;
    int wall_hour = 0;
    int wall_minute = 0;
    int wall_second = 0;
#if defined(__unix__) || defined(__APPLE__)
    time_t seconds = (time_t)(event->wall_time_ns / CLOGX_NS_PER_SECOND);
    struct tm local_tm;
    memset(&local_tm, 0, sizeof(local_tm));
    if (localtime_r(&seconds, &local_tm)) {
        wall_hour = local_tm.tm_hour;
        wall_minute = local_tm.tm_min;
        wall_second = local_tm.tm_sec;
    }
#else
    /* ISO C has no thread-safe local-time conversion. Use UTC time-of-day on the
     * dependency-free fallback rather than racing localtime()'s shared object. */
    uint64_t seconds_in_day = (event->wall_time_ns / CLOGX_NS_PER_SECOND) % UINT64_C(86400);
    wall_hour = (int)(seconds_in_day / UINT64_C(3600));
    wall_minute = (int)((seconds_in_day / UINT64_C(60)) % UINT64_C(60));
    wall_second = (int)(seconds_in_day % UINT64_C(60));
#endif
    if (flags & CLOGX_FLAG_PRINT_TIME) {
        uint64_t elapsed_ms = event->monotonic_time_ns / CLOGX_NS_PER_MILLISECOND;
        int length = snprintf(metadata + metadata_len, sizeof(metadata) - metadata_len,
                              "%02d:%02d:%02d.%03llu +%06llu.%03llu ", wall_hour, wall_minute,
                              wall_second, (unsigned long long)wall_ms,
                              (unsigned long long)(elapsed_ms / 1000u),
                              (unsigned long long)(elapsed_ms % 1000u));
        if (length > 0) {
            metadata_len += (size_t)length < sizeof(metadata) - metadata_len
                                ? (size_t)length
                                : strlen(metadata + metadata_len);
        }
    }
    if ((flags & CLOGX_FLAG_PRINT_LEVEL) && metadata_len < sizeof(metadata) - 1u) {
        int length = snprintf(metadata + metadata_len, sizeof(metadata) - metadata_len, "%s ",
                              clogx_level_name(event->level));
        if (length > 0) {
            metadata_len += (size_t)length < sizeof(metadata) - metadata_len
                                ? (size_t)length
                                : strlen(metadata + metadata_len);
        }
    }
    if ((flags & CLOGX_FLAG_PRINT_CATEGORY) && metadata_len < sizeof(metadata) - 1u) {
        int length = snprintf(metadata + metadata_len, sizeof(metadata) - metadata_len, "%s: ",
                              event->category ? event->category : "unknown");
        if (length > 0) {
            metadata_len += (size_t)length < sizeof(metadata) - metadata_len
                                ? (size_t)length
                                : strlen(metadata + metadata_len);
        }
    }

    size_t offset = 0;
    offset = clogx_append(output, capacity, offset, metadata, metadata_len);
    bool automatic_source = event->level <= CLOGX_LEVEL_DEBUG ||
                            (event->category && strncmp(event->category, "diagnostics.", 12) == 0);
    bool show_source = (flags & CLOGX_FLAG_PRINT_SOURCE) != 0 ||
                       ((flags & CLOGX_FLAG_AUTO_SOURCE) != 0 && automatic_source);
    if (show_source && event->file && event->function) {
        char source[320];
        int source_len = snprintf(source, sizeof(source), "%s:%d %s: ", event->file, event->line, event->function);
        if (source_len > 0) {
            if ((size_t)source_len >= sizeof(source)) {
                source_len = (int)strlen(source);
            }
            offset = clogx_append(output, capacity, offset, source, (size_t)source_len);
        }
    }
    const char *message = event->message ? event->message : "";
    offset = clogx_append(output, capacity, offset, message, strlen(message));
    offset = clogx_append(output, capacity, offset, "\n", 1u);
    if (capacity > 0) {
        if (offset < capacity) {
            output[offset] = '\0';
        } else if (capacity > sizeof(" <truncated>\n")) {
            static const char marker[] = " <truncated>\n";
            memcpy(output + capacity - sizeof(marker), marker, sizeof(marker));
        } else {
            output[capacity - 1u] = '\0';
        }
    }
    return offset;
}

static void clogx_default_sink(const ClogxEvent *event) {
    char line[CLOGX_LINE_CAPACITY];
    (void)clogx_format_event(event, line, sizeof(line));
    (void)fwrite(line, 1, strlen(line), stderr);
}

static void clogx_deliver(const ClogxEvent *event) {
    clogx_lock(&g_state_lock);
    unsigned slot_index = g_active_sink_slot;
    ClogxSink sink = g_sink_slots[slot_index].sink;
    void *context = g_sink_slots[slot_index].context;
    if (sink) {
        g_sink_slots[slot_index].in_flight++;
    }
    clogx_unlock(&g_state_lock);
    if (sink) {
        sink(event, context);
        clogx_lock(&g_state_lock);
        g_sink_slots[slot_index].in_flight--;
        clogx_unlock(&g_state_lock);
    } else {
        clogx_default_sink(event);
    }
}

static void clogx_mark_truncated(char *message, size_t capacity) {
    static const char marker[] = " <truncated>";
    size_t marker_len = sizeof(marker) - 1u;
    if (capacity <= marker_len + 1u) {
        if (capacity > 0) {
            message[capacity - 1u] = '\0';
        }
        return;
    }
    size_t start = capacity - 1u - marker_len;
    memcpy(message + start, marker, marker_len + 1u);
}

static void clogx_sanitize_message(char *message) {
    size_t len = strlen(message);
    while (len > 0 && (message[len - 1] == '\n' || message[len - 1] == '\r')) {
        message[--len] = '\0';
    }
    for (size_t i = 0; i < len; i++) {
        if (message[i] == '\n' || message[i] == '\r') {
            message[i] = ' ';
        }
    }
}

static bool clogx_vformat_message(char *message, size_t capacity, const char *format, va_list args) {
    if (!message || capacity == 0) {
        return true;
    }
    if (!format) {
        message[0] = '\0';
        return false;
    }
    int result = vsnprintf(message, capacity, format, args);
    bool truncated = result < 0 || (size_t)result >= capacity;
    if (result < 0) {
        (void)snprintf(message, capacity, "<format-error>");
    } else if (truncated) {
        clogx_mark_truncated(message, capacity);
    }
    clogx_sanitize_message(message);
    return truncated;
}

static void clogx_vlogf_internal(ClogxCategory *category, ClogxLevel level, const char *file, int line,
                                 const char *function, const char *format, va_list args, bool force) {
    if (!category || !format || (!force && !clogx_enabled(category, level))) {
        return;
    }
    if (force) {
        clogx_register_category(category);
    }

    char message[CLOGX_MESSAGE_CAPACITY];
    bool truncated = clogx_vformat_message(message, sizeof(message), format, args);

    ClogxEvent event = {
        .level = level,
        .flags = category->flags,
        .category = category->name,
        .message = message,
        .file = file,
        .function = function,
        .line = line,
        .wall_time_ns = clogx_wall_now_ns(),
        .monotonic_time_ns = clogx_monotonic_elapsed_ns(),
        .message_truncated = truncated,
    };
    clogx_deliver(&event);
}

void clogx_vlogf(ClogxCategory *category, ClogxLevel level, const char *file, int line,
                  const char *function, const char *format, va_list args) {
    clogx_vlogf_internal(category, level, file, line, function, format, args, false);
}

void clogx_logf(ClogxCategory *category, ClogxLevel level, const char *file, int line,
                 const char *function, const char *format, ...) {
    va_list args;
    va_start(args, format);
    clogx_vlogf(category, level, file, line, function, format, args);
    va_end(args);
}

static void clogx_logf_force(ClogxCategory *category, ClogxLevel level, const char *file, int line,
                             const char *function, const char *format, ...) {
    va_list args;
    va_start(args, format);
    clogx_vlogf_internal(category, level, file, line, function, format, args, true);
    va_end(args);
}

bool clogx_rate_limit_allow(ClogxRateLimit *limit, uint32_t burst, uint64_t interval_ms,
                            uint32_t *suppressed) {
    if (suppressed) {
        *suppressed = 0;
    }
    if (!limit || burst == 0 || interval_ms == 0) {
        return true;
    }
    uint64_t interval_ns = interval_ms > UINT64_MAX / CLOGX_NS_PER_MILLISECOND
                               ? UINT64_MAX
                               : interval_ms * CLOGX_NS_PER_MILLISECOND;
    uint64_t now = clogx_monotonic_raw_ns();
    clogx_lock(&limit->lock);
    if (limit->emitted == 0 || now < limit->window_start_ns ||
        now - limit->window_start_ns >= interval_ns) {
        if (suppressed) {
            *suppressed = limit->suppressed;
        }
        limit->window_start_ns = now;
        limit->emitted = 1;
        limit->suppressed = 0;
        clogx_unlock(&limit->lock);
        return true;
    }
    if (limit->emitted < burst) {
        limit->emitted++;
        clogx_unlock(&limit->lock);
        return true;
    }
    if (limit->suppressed != UINT32_MAX) {
        limit->suppressed++;
    }
    clogx_unlock(&limit->lock);
    return false;
}

void clogx_logf_suppressed(ClogxCategory *category, ClogxLevel level, const char *file, int line,
                           const char *function, uint32_t suppressed, const char *format, ...) {
    if (suppressed > 0) {
        clogx_logf(category, level, file, line, function, "suppressed %" PRIu32 " similar messages", suppressed);
    }
    va_list args;
    va_start(args, format);
    clogx_vlogf(category, level, file, line, function, format, args);
    va_end(args);
}

static void clogx_emit_backtrace(const char *file, int line, const char *function, bool force) {
#if defined(CLOGX_HAVE_EXECINFO) && CLOGX_HAVE_EXECINFO
    void *frames[CLOGX_BACKTRACE_FRAMES];
    int count = backtrace(frames, CLOGX_BACKTRACE_FRAMES);
    char **symbols = count > 0 ? backtrace_symbols(frames, count) : NULL;
    for (int i = 2; i < count; i++) {
        if (force) {
            if (symbols) {
                clogx_logf_force(&g_diagnostics_backtrace, CLOGX_LEVEL_ERROR, file, line, function,
                                 "#%d %s", i - 2, symbols[i]);
            } else {
                clogx_logf_force(&g_diagnostics_backtrace, CLOGX_LEVEL_ERROR, file, line, function,
                                 "#%d %p", i - 2, frames[i]);
            }
        } else {
            if (symbols) {
                clogx_logf(&g_diagnostics_backtrace, CLOGX_LEVEL_ERROR, file, line, function,
                           "#%d %s", i - 2, symbols[i]);
            } else {
                clogx_logf(&g_diagnostics_backtrace, CLOGX_LEVEL_ERROR, file, line, function,
                           "#%d %p", i - 2, frames[i]);
            }
        }
    }
    free(symbols);
#else
    (void)file;
    (void)line;
    (void)function;
    (void)force;
#endif
}

#if defined(__linux__)
static bool clogx_debugger_attached(void) {
    FILE *status = fopen("/proc/self/status", "r");
    if (!status) {
        return false;
    }
    char line[128];
    bool attached = false;
    while (fgets(line, sizeof(line), status)) {
        if (strncmp(line, "TracerPid:", 10) == 0) {
            attached = strtoul(line + 10, NULL, 10) != 0;
            break;
        }
    }
    fclose(status);
    return attached;
}
#endif

static bool clogx_break_enabled(ClogxLevel trigger_level) {
    return clogx_enabled(&g_diagnostics_break, trigger_level);
}

static void clogx_maybe_break(ClogxLevel trigger_level) {
#if defined(__linux__)
    if (clogx_break_enabled(trigger_level) && clogx_debugger_attached()) {
        (void)raise(SIGTRAP);
    }
#else
    (void)trigger_level;
#endif
}

void clogx_assert_fail(ClogxCategory *category, const char *expression, const char *file, int line,
                       const char *function, const char *format, ...) {
    char detail[CLOGX_MESSAGE_CAPACITY] = "";
    if (format) {
        va_list args;
        va_start(args, format);
        (void)clogx_vformat_message(detail, sizeof(detail), format, args);
        va_end(args);
    }
    const char *source_category = category && category->name ? category->name : "unknown";
    if (detail[0]) {
        clogx_logf(&g_diagnostics_assert, CLOGX_LEVEL_ERROR, file, line, function,
                   "%s: assertion failed: %s (%s)", source_category, expression ? expression : "?", detail);
    } else {
        clogx_logf(&g_diagnostics_assert, CLOGX_LEVEL_ERROR, file, line, function,
                   "%s: assertion failed: %s", source_category, expression ? expression : "?");
    }
    if (clogx_enabled(&g_diagnostics_backtrace, CLOGX_LEVEL_ERROR)) {
        clogx_emit_backtrace(file, line, function, false);
    }
    clogx_maybe_break(CLOGX_LEVEL_ERROR);
}

_Noreturn void clogx_panicf(ClogxCategory *category, const char *file, int line,
                            const char *function, const char *format, ...) {
    char detail[CLOGX_MESSAGE_CAPACITY];
    va_list args;
    va_start(args, format);
    (void)clogx_vformat_message(detail, sizeof(detail), format ? format : "panic", args);
    va_end(args);
    ClogxCategory *output_category = category ? category : &g_diagnostics_assert;
    clogx_logf_force(output_category, CLOGX_LEVEL_FATAL, file, line, function, "%s", detail);
    clogx_emit_backtrace(file, line, function, true);
    clogx_maybe_break(CLOGX_LEVEL_FATAL);
    abort();
}

#ifdef CLOGX_TESTING
void clogx_test_set_clocks(ClogxTestClock wall_clock, ClogxTestClock monotonic_clock) {
    clogx_lock(&g_clock_lock);
    g_test_wall_clock = wall_clock;
    g_test_monotonic_clock = monotonic_clock;
    g_monotonic_origin_ns = 0;
    g_monotonic_fallback_last_ns = 0;
    clogx_unlock(&g_clock_lock);
}

size_t clogx_test_format_event(const ClogxEvent *event, char *output, size_t capacity) {
    return clogx_format_event(event, output, capacity);
}

bool clogx_test_break_enabled(ClogxLevel trigger_level) {
    return clogx_break_enabled(trigger_level);
}
#endif
