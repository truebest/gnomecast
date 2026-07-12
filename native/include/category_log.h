/* SPDX-License-Identifier: MIT */
#ifndef CATEGORY_LOG_H
#define CATEGORY_LOG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#if defined(__GNUC__) || defined(__clang__)
#define CLOGX_PRINTF_FORMAT(format_index_, first_argument_) \
    __attribute__((format(printf, format_index_, first_argument_)))
#else
#define CLOGX_PRINTF_FORMAT(format_index_, first_argument_)
#endif

#define CLOGX_LEVEL_VALUE_TRACE 0
#define CLOGX_LEVEL_VALUE_DEBUG 1
#define CLOGX_LEVEL_VALUE_INFO 2
#define CLOGX_LEVEL_VALUE_NOTICE 3
#define CLOGX_LEVEL_VALUE_WARN 4
#define CLOGX_LEVEL_VALUE_ERROR 5
#define CLOGX_LEVEL_VALUE_FATAL 6
#define CLOGX_LEVEL_VALUE_OFF 7

typedef enum ClogxLevel {
    CLOGX_LEVEL_TRACE = CLOGX_LEVEL_VALUE_TRACE,
    CLOGX_LEVEL_DEBUG = CLOGX_LEVEL_VALUE_DEBUG,
    CLOGX_LEVEL_INFO = CLOGX_LEVEL_VALUE_INFO,
    CLOGX_LEVEL_NOTICE = CLOGX_LEVEL_VALUE_NOTICE,
    CLOGX_LEVEL_WARN = CLOGX_LEVEL_VALUE_WARN,
    CLOGX_LEVEL_ERROR = CLOGX_LEVEL_VALUE_ERROR,
    CLOGX_LEVEL_FATAL = CLOGX_LEVEL_VALUE_FATAL,
    CLOGX_LEVEL_OFF = CLOGX_LEVEL_VALUE_OFF
} ClogxLevel;

typedef uint32_t ClogxFlags;

enum {
    CLOGX_FLAG_NONE = 0u,
    /* Print wall time and monotonic time since the logger's first use. */
    CLOGX_FLAG_PRINT_TIME = 1u << 0,
    CLOGX_FLAG_PRINT_LEVEL = 1u << 1,
    /* Print the category name as the message prefix. */
    CLOGX_FLAG_PRINT_CATEGORY = 1u << 2,
    /* Always print file, line, and function metadata for this category. */
    CLOGX_FLAG_PRINT_SOURCE = 1u << 3,
    /* Print source automatically for trace/debug and diagnostic events. */
    CLOGX_FLAG_AUTO_SOURCE = 1u << 4,
    CLOGX_FLAGS_DEFAULT = CLOGX_FLAG_PRINT_TIME | CLOGX_FLAG_PRINT_LEVEL |
                          CLOGX_FLAG_PRINT_CATEGORY | CLOGX_FLAG_AUTO_SOURCE
};

#define CLOGX_FLAG_DEFAULT CLOGX_FLAGS_DEFAULT
#define CLOGX_FLAG_PRINT_PREFIX CLOGX_FLAG_PRINT_CATEGORY

typedef struct ClogxCategory {
    /* Public for static initialization; treat every field as logger-owned after definition. */
    const char *name;
    ClogxLevel default_level;
    ClogxFlags flags;
    /* Reserved for category-specific immutable configuration; the core does not dereference it. */
    const void *config;
    atomic_int effective_level;
    atomic_bool registered;
    struct ClogxCategory *next;
} ClogxCategory;

/* Categories and their name strings must have process lifetime (normally file scope):
 * first use permanently registers the object for live reconfiguration. */
#define CLOGX_CATEGORY_INITIALIZER_FULL(name_, default_level_, flags_, config_)                              \
    { (name_), (default_level_), (flags_), (config_), (default_level_), false, NULL }
#define CLOGX_CATEGORY_INITIALIZER(name_, default_level_)                                                    \
    CLOGX_CATEGORY_INITIALIZER_FULL((name_), (default_level_), CLOGX_FLAGS_DEFAULT, NULL)
#define CLOGX_CATEGORY_DEFINE(symbol_, name_, default_level_)                                                \
    ClogxCategory symbol_ = CLOGX_CATEGORY_INITIALIZER((name_), (default_level_))
#define CLOGX_CATEGORY_DECLARE(symbol_) extern ClogxCategory symbol_

typedef struct ClogxEvent {
    ClogxLevel level;
    ClogxFlags flags;
    const char *category;
    const char *message;
    const char *file;
    const char *function;
    int line;
    uint64_t wall_time_ns;
    uint64_t monotonic_time_ns;
    bool message_truncated;
} ClogxEvent;

/* monotonic_time_ns uses a steady platform clock when one is available. The ISO C
 * fallback is nondecreasing and sleep-aware, but wall-clock adjustments can move it
 * forward; configure-time feature detection should enable the steady clock normally. */

/* The callback is synchronous and may run concurrently on several logging threads. Event
 * strings remain valid only until the callback returns. A sink must not log recursively
 * or replace/reset itself. Replacing a sink waits for old callbacks to drain, so its old
 * context may be released after clogx_set_sink/clogx_reset_sink returns. */
typedef void (*ClogxSink)(const ClogxEvent *event, void *context);

enum {
    CLOGX_CONFIG_OK = 0,
    CLOGX_CONFIG_INVALID = -1,
    CLOGX_CONFIG_TOO_MANY_RULES = -2
};

const char *clogx_level_name(ClogxLevel level);
bool clogx_enabled(ClogxCategory *category, ClogxLevel level);
bool clogx_enabled_at_or_above(ClogxCategory *category, ClogxLevel level, int compiled_min_level);

/* Apply up to 32 comma-separated, case-sensitive selector=level rules transactionally.
 * A selector also matches dot-separated descendants; later rules win. Empty resets all
 * categories to their compiled defaults. */
int clogx_configure(const char *spec);
/* Apply GNOMECAST_LOG, or reset to defaults when it is absent. */
int clogx_configure_env(void);
void clogx_set_sink(ClogxSink sink, void *context);
void clogx_reset_sink(void);

void clogx_vlogf(ClogxCategory *category, ClogxLevel level, const char *file, int line,
                  const char *function, const char *format, va_list args)
    CLOGX_PRINTF_FORMAT(6, 0);
void clogx_logf(ClogxCategory *category, ClogxLevel level, const char *file, int line,
                 const char *function, const char *format, ...)
    CLOGX_PRINTF_FORMAT(6, 7);

#ifndef CLOGX_COMPILED_MIN_LEVEL
#define CLOGX_COMPILED_MIN_LEVEL CLOGX_LEVEL_VALUE_TRACE
#endif
/* Override with a preprocessor value, for example
 * -DCLOGX_COMPILED_MIN_LEVEL=CLOGX_LEVEL_VALUE_INFO. Enum identifiers cannot be used in
 * preprocessor comparisons. Fixed-level macros below that floor compile to no-ops. */

#define CLOGX_LOG(category_, level_, ...)                                                                    \
    do {                                                                                                     \
        ClogxCategory *clogx_category__ = (category_);                                                       \
        ClogxLevel clogx_level__ = (level_);                                                                 \
        if ((int)clogx_level__ >= (int)CLOGX_COMPILED_MIN_LEVEL &&                                          \
            clogx_enabled(clogx_category__, clogx_level__)) {                                               \
            clogx_logf(clogx_category__, clogx_level__, __FILE__, __LINE__, __func__, __VA_ARGS__);         \
        }                                                                                                    \
    } while (0)

#if CLOGX_COMPILED_MIN_LEVEL <= CLOGX_LEVEL_VALUE_TRACE
#define CLOGX_TRACE(category_, ...) CLOGX_LOG((category_), CLOGX_LEVEL_TRACE, __VA_ARGS__)
#else
#define CLOGX_TRACE(category_, ...) ((void)0)
#endif
#if CLOGX_COMPILED_MIN_LEVEL <= CLOGX_LEVEL_VALUE_DEBUG
#define CLOGX_DEBUG(category_, ...) CLOGX_LOG((category_), CLOGX_LEVEL_DEBUG, __VA_ARGS__)
#else
#define CLOGX_DEBUG(category_, ...) ((void)0)
#endif
#if CLOGX_COMPILED_MIN_LEVEL <= CLOGX_LEVEL_VALUE_INFO
#define CLOGX_INFO(category_, ...) CLOGX_LOG((category_), CLOGX_LEVEL_INFO, __VA_ARGS__)
#else
#define CLOGX_INFO(category_, ...) ((void)0)
#endif
#if CLOGX_COMPILED_MIN_LEVEL <= CLOGX_LEVEL_VALUE_NOTICE
#define CLOGX_NOTICE(category_, ...) CLOGX_LOG((category_), CLOGX_LEVEL_NOTICE, __VA_ARGS__)
#else
#define CLOGX_NOTICE(category_, ...) ((void)0)
#endif
#if CLOGX_COMPILED_MIN_LEVEL <= CLOGX_LEVEL_VALUE_WARN
#define CLOGX_WARN(category_, ...) CLOGX_LOG((category_), CLOGX_LEVEL_WARN, __VA_ARGS__)
#else
#define CLOGX_WARN(category_, ...) ((void)0)
#endif
#if CLOGX_COMPILED_MIN_LEVEL <= CLOGX_LEVEL_VALUE_ERROR
#define CLOGX_ERROR(category_, ...) CLOGX_LOG((category_), CLOGX_LEVEL_ERROR, __VA_ARGS__)
#else
#define CLOGX_ERROR(category_, ...) ((void)0)
#endif
#if CLOGX_COMPILED_MIN_LEVEL <= CLOGX_LEVEL_VALUE_FATAL
#define CLOGX_FATAL(category_, ...) CLOGX_LOG((category_), CLOGX_LEVEL_FATAL, __VA_ARGS__)
#else
#define CLOGX_FATAL(category_, ...) ((void)0)
#endif

typedef struct ClogxRateLimit {
    atomic_flag lock;
    uint64_t window_start_ns;
    uint32_t emitted;
    uint32_t suppressed;
} ClogxRateLimit;

#define CLOGX_RATE_LIMIT_INITIALIZER { ATOMIC_FLAG_INIT, 0, 0, 0 }

bool clogx_rate_limit_allow(ClogxRateLimit *limit, uint32_t burst, uint64_t interval_ms,
                            uint32_t *suppressed);
void clogx_logf_suppressed(ClogxCategory *category, ClogxLevel level, const char *file, int line,
                           const char *function, uint32_t suppressed, const char *format, ...)
    CLOGX_PRINTF_FORMAT(7, 8);

/* Each macro expansion owns an independent, thread-safe call-site limiter. */
#define CLOGX_LOG_LIMITED(category_, level_, burst_, interval_ms_, ...)                                      \
    do {                                                                                                     \
        static ClogxRateLimit clogx_rate_limit__ = CLOGX_RATE_LIMIT_INITIALIZER;                            \
        ClogxCategory *clogx_category__ = (category_);                                                       \
        ClogxLevel clogx_level__ = (level_);                                                                 \
        uint32_t clogx_suppressed__ = 0;                                                                     \
        if ((int)clogx_level__ >= (int)CLOGX_COMPILED_MIN_LEVEL &&                                          \
            clogx_enabled(clogx_category__, clogx_level__) &&                                               \
            clogx_rate_limit_allow(&clogx_rate_limit__, (burst_), (interval_ms_),                            \
                                   &clogx_suppressed__)) {                                                   \
            clogx_logf_suppressed(clogx_category__, clogx_level__, __FILE__, __LINE__, __func__,            \
                                  clogx_suppressed__, __VA_ARGS__);                                         \
        }                                                                                                    \
    } while (0)

void clogx_assert_fail(ClogxCategory *category, const char *expression, const char *file, int line,
                       const char *function, const char *format, ...)
    CLOGX_PRINTF_FORMAT(6, 7);
_Noreturn void clogx_panicf(ClogxCategory *category, const char *file, int line,
                            const char *function, const char *format, ...)
    CLOGX_PRINTF_FORMAT(5, 6);

#ifndef NDEBUG
#define CLOGX_ASSERT(category_, expression_)                                                                 \
    do {                                                                                                     \
        if (!(expression_)) {                                                                                \
            clogx_assert_fail((category_), #expression_, __FILE__, __LINE__, __func__, NULL);               \
        }                                                                                                    \
    } while (0)
#define CLOGX_ASSERT_MSG(category_, expression_, ...)                                                        \
    do {                                                                                                     \
        if (!(expression_)) {                                                                                \
            clogx_assert_fail((category_), #expression_, __FILE__, __LINE__, __func__, __VA_ARGS__);        \
        }                                                                                                    \
    } while (0)
#else
#define CLOGX_ASSERT(category_, expression_) ((void)0)
#define CLOGX_ASSERT_MSG(category_, expression_, ...) ((void)0)
#endif

#define CLOGX_PANIC(category_, ...)                                                                          \
    clogx_panicf((category_), __FILE__, __LINE__, __func__, __VA_ARGS__)

#ifdef CLOGX_TESTING
typedef uint64_t (*ClogxTestClock)(void);
void clogx_test_set_clocks(ClogxTestClock wall_clock, ClogxTestClock monotonic_clock);
size_t clogx_test_format_event(const ClogxEvent *event, char *output, size_t capacity);
bool clogx_test_break_enabled(ClogxLevel trigger_level);
#endif

#endif
