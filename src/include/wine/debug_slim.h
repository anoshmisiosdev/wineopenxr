/* SPDX-License-Identifier: LGPL-2.1-or-later */

/*
 * Wine debug channel API for Objective-C TUs that cannot include the full
 * <wine/debug.h>. Pulling windef.h / winbase.h / guiddef.h redefines BOOL
 * as int, which clashes with Objective-C's signed-char BOOL on macOS.
 *
 * Shape and symbol names match wine/debug.h exactly so WINEDEBUG=+openxr
 * lights up traces in both full-header and slim-header TUs
 */
#ifndef __WINE_WINE_DEBUG_SLIM_H
#define __WINE_WINE_DEBUG_SLIM_H

#include <stdarg.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

enum __wine_debug_class
{
    __WINE_DBCL_FIXME,
    __WINE_DBCL_ERR,
    __WINE_DBCL_WARN,
    __WINE_DBCL_TRACE,
    __WINE_DBCL_INIT = 7
};

struct __wine_debug_channel
{
    unsigned char flags;
    char name[15];
};

#ifndef WINE_NO_TRACE_MSGS
# define __WINE_GET_DEBUGGING_TRACE(dbch) ((dbch)->flags & (1 << __WINE_DBCL_TRACE))
#else
# define __WINE_GET_DEBUGGING_TRACE(dbch) 0
#endif

#ifndef WINE_NO_DEBUG_MSGS
# define __WINE_GET_DEBUGGING_WARN(dbch)  ((dbch)->flags & (1 << __WINE_DBCL_WARN))
# define __WINE_GET_DEBUGGING_FIXME(dbch) ((dbch)->flags & (1 << __WINE_DBCL_FIXME))
#else
# define __WINE_GET_DEBUGGING_WARN(dbch)  0
# define __WINE_GET_DEBUGGING_FIXME(dbch) 0
#endif

#define __WINE_GET_DEBUGGING_ERR(dbch)   ((dbch)->flags & (1 << __WINE_DBCL_ERR))
#define __WINE_GET_DEBUGGING(dbcl, dbch) __WINE_GET_DEBUGGING##dbcl(dbch)

extern int __cdecl __wine_dbg_output(const char *str);
extern int __cdecl __wine_dbg_header(enum __wine_debug_class cls,
                                     struct __wine_debug_channel *channel,
                                     const char *function);

static inline int __wine_dbg_slim_vprintf(const char *format, va_list args)
{
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    buffer[sizeof(buffer) - 1] = 0;
    return __wine_dbg_output(buffer);
}

static inline int __wine_dbg_slim_log(enum __wine_debug_class cls,
                                      struct __wine_debug_channel *channel,
                                      const char *function,
                                      const char *format, ...)
    __attribute__((format(printf, 4, 5)));
static inline int __wine_dbg_slim_log(enum __wine_debug_class cls,
                                      struct __wine_debug_channel *channel,
                                      const char *function,
                                      const char *format, ...)
{
    va_list args;
    int ret;

    if (__wine_dbg_header(cls, channel, function) == -1)
        return 0;
    va_start(args, format);
    ret = __wine_dbg_slim_vprintf(format, args);
    va_end(args);
    return ret;
}

#define __WINE_DPRINTF(dbcl, dbch)                                       \
    do { if (__WINE_GET_DEBUGGING(dbcl, (dbch))) {                       \
        struct __wine_debug_channel * const __dbch = (dbch);             \
        const enum __wine_debug_class __dbcl = __WINE_DBCL##dbcl;        \
        __WINE_DBG_LOG
#define __WINE_DBG_LOG(...) \
    __wine_dbg_slim_log(__dbcl, __dbch, __func__, __VA_ARGS__); } } while (0)

#define WINE_TRACE       __WINE_DPRINTF(_TRACE, __wine_dbch___default)
#define WINE_TRACE_(ch)  __WINE_DPRINTF(_TRACE, &__wine_dbch_##ch)
#define WINE_WARN        __WINE_DPRINTF(_WARN,  __wine_dbch___default)
#define WINE_WARN_(ch)   __WINE_DPRINTF(_WARN,  &__wine_dbch_##ch)
#define WINE_ERR         __WINE_DPRINTF(_ERR,   __wine_dbch___default)
#define WINE_ERR_(ch)    __WINE_DPRINTF(_ERR,   &__wine_dbch_##ch)
#define WINE_FIXME       __WINE_DPRINTF(_FIXME, __wine_dbch___default)
#define WINE_FIXME_(ch)  __WINE_DPRINTF(_FIXME, &__wine_dbch_##ch)

#define WINE_DECLARE_DEBUG_CHANNEL(ch)                                    \
    static struct __wine_debug_channel __wine_dbch_##ch = { 0xff, #ch };  \
    _Static_assert(sizeof(#ch) <= sizeof(((struct __wine_debug_channel *)0)->name), \
                   "wine debug channel name too long")
#define WINE_DEFAULT_DEBUG_CHANNEL(ch)                                    \
    static struct __wine_debug_channel __wine_dbch_##ch = { 0xff, #ch };  \
    _Static_assert(sizeof(#ch) <= sizeof(((struct __wine_debug_channel *)0)->name), \
                   "wine debug channel name too long");                   \
    static struct __wine_debug_channel * const __wine_dbch___default = &__wine_dbch_##ch

#ifdef __cplusplus
}
#endif

#endif /* __WINE_WINE_DEBUG_SLIM_H */
