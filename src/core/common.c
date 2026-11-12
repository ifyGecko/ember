/*
 * common.c - Status strings and logging.
 */
#include <stdarg.h>
#include <stdio.h>

#include "uam/common.h"

uam_log_level uam_log_threshold = UAM_LOG_WARN;
bool uam_log_stamp;

static uam_origin_fn g_origin_fn;
static void *g_origin_ctx;

void uam_origin_set_provider(uam_origin_fn fn, void *ctx, uam_origin_fn *prev_fn, void **prev_ctx)
{
    if (prev_fn)
        *prev_fn = g_origin_fn;
    if (prev_ctx)
        *prev_ctx = g_origin_ctx;
    g_origin_fn = fn;
    g_origin_ctx = fn ? ctx : NULL;
}

bool uam_origin_get(uam_origin *out)
{
    return g_origin_fn && g_origin_fn(g_origin_ctx, out);
}

char *uam_origin_format(char *buf, size_t len)
{
    uam_origin o;
    if (!len)
        return buf;
    buf[0] = 0;
    if (!uam_origin_get(&o))
        return buf;
    if (o.cpu)
        snprintf(buf, len, " pc=0x%llx icount=%llu cpu=%u", (unsigned long long)o.pc,
                 (unsigned long long)o.icount, o.cpu);
    else
        snprintf(buf, len, " pc=0x%llx icount=%llu", (unsigned long long)o.pc,
                 (unsigned long long)o.icount);
    return buf;
}

const char *uam_status_str(uam_status st)
{
    switch (st) {
    case UAM_OK:             return "ok";
    case UAM_ERR_FAULT:      return "access fault";
    case UAM_ERR_XLATE:      return "translation fault";
    case UAM_ERR_MISALIGNED: return "misaligned";
    case UAM_ERR_ILLEGAL:    return "illegal";
    case UAM_ERR_NOMEM:      return "out of memory";
    case UAM_ERR_INVALID:    return "invalid";
    case UAM_ERR_REJECTED:   return "input rejected";
    }
    return "?";
}

unsigned uam_abi_version(void)
{
    return UAM_ABI_VERSION;
}

void uam_log(uam_log_level lvl, const char *module, const char *fmt, ...)
{
    static const char *const names[] = { "error", "warn", "info", "debug" };
    if (lvl > uam_log_threshold)
        return;
    va_list ap;
    va_start(ap, fmt);
    uam_origin o;
    if (uam_log_stamp && uam_origin_get(&o)) {
        if (o.cpu)
            fprintf(stderr, "@%llu:cpu%u ", (unsigned long long)o.icount, o.cpu);
        else
            fprintf(stderr, "@%llu ", (unsigned long long)o.icount);
    }
    fprintf(stderr, "[%s] %s: ", names[lvl], module);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
