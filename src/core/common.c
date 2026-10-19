/*
 * common.c - Status strings and logging.
 */
#include <stdarg.h>
#include <stdio.h>

#include "uam/common.h"

uam_log_level uam_log_threshold = UAM_LOG_WARN;

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
    fprintf(stderr, "[%s] %s: ", names[lvl], module);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
