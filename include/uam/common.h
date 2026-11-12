/*
 * uam/common.h - Basic types shared by every UAM module.
 *
 * This header has no dependencies beyond the C standard library. It defines
 * the status codes used across interfaces and a few bit-vector helpers that
 * implement the width rules of the IR (docs/uam-isa.md section 3.1).
 */
#ifndef UAM_COMMON_H
#define UAM_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Result of an operation that crosses a module interface. */
typedef enum uam_status {
    UAM_OK = 0,
    UAM_ERR_FAULT,      /* bus fault: unmapped physical address, device error */
    UAM_ERR_XLATE,      /* address translation fault: no valid mapping, protection,
                         * or an address the current privilege may not use */
    UAM_ERR_MISALIGNED, /* alignment violation on an ALIGNED access */
    UAM_ERR_ILLEGAL,    /* system register / operation not permitted */
    UAM_ERR_NOMEM,      /* host allocation failure */
    UAM_ERR_INVALID,    /* malformed input (bad IR, bad image, bad config) */
    UAM_ERR_REJECTED,   /* a fuzz input route cannot deliver this input (too long, wrong
                         * shape): the input's doing, not a harness error (uam/fuzz.h) */
} uam_status;

const char *uam_status_str(uam_status st);

/* All-ones mask for a width in bits (1..64). */
static inline uint64_t uam_mask(unsigned width)
{
    return width >= 64 ? ~UINT64_C(0) : (UINT64_C(1) << width) - 1;
}

/* Sign-extend the low `width` bits of v to 64 bits. */
static inline int64_t uam_sext(uint64_t v, unsigned width)
{
    if (width >= 64)
        return (int64_t)v;
    uint64_t sign = UINT64_C(1) << (width - 1);
    v &= uam_mask(width);
    return (int64_t)((v ^ sign) - sign);
}

/*
 * Version of the binary interface between libuam and code compiled against
 * these headers (boards, devices, host backends). Bump it whenever a public
 * struct's layout, an enum's values, or a public function's signature
 * changes. Entry points check it together with the struct sizes the caller
 * was compiled with (uam_abi_layout in uam/machine.h).
 */
#define UAM_ABI_VERSION 22u

/* The UAM_ABI_VERSION the library itself was built with. */
unsigned uam_abi_version(void);

#define UAM_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* Recover the enclosing struct from a pointer to one of its members. */
#define UAM_CONTAINER_OF(ptr, type, member) \
    ((type *)(void *)((char *)(ptr) - offsetof(type, member)))

/* Minimal logging used by all modules; writes to stderr. With uam_log_stamp set
 * (the runner's --log-stamp), each line made while a processor runs starts with where
 * the guest stands: "@ICOUNT" ("@ICOUNT:cpuN" for processors other than 0). */
typedef enum { UAM_LOG_ERROR, UAM_LOG_WARN, UAM_LOG_INFO, UAM_LOG_DEBUG } uam_log_level;
extern uam_log_level uam_log_threshold;
extern bool uam_log_stamp;
void uam_log(uam_log_level lvl, const char *module, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/*
 * The origin of what is happening now, for diagnostics: the guest instruction a
 * processor is executing (exact while it makes a bus access, a system register access
 * or a helper call; between blocks - timers, interrupts, hooks - the next one to run) and the processor's
 * instruction count. A device's log line can then say which code touched it without the
 * device knowing anything about processors. The executor publishes its processors while
 * it runs (uam_exec_run); with nothing running (the loader, a debugger, the board being
 * built) uam_origin_get returns false.
 */
typedef struct uam_origin {
    unsigned cpu;      /* processor number (hartid) */
    uint64_t pc;       /* the instruction */
    uint64_t icount;   /* that processor's instruction count */
} uam_origin;

typedef bool (*uam_origin_fn)(void *ctx, uam_origin *out);
bool   uam_origin_get(uam_origin *out);
/* Install the provider; returns the previous one in *prev_fn / *prev_ctx (may be NULL)
 * so that a nested run can put it back. */
void   uam_origin_set_provider(uam_origin_fn fn, void *ctx, uam_origin_fn *prev_fn, void **prev_ctx);
/* " pc=0x... icount=N" (and " cpu=N" for processors other than 0) into buf, or "" when
 * nothing runs. Returns buf. */
char  *uam_origin_format(char *buf, size_t len);

#endif /* UAM_COMMON_H */
