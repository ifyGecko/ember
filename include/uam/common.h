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
#define UAM_ABI_VERSION 4u

/* The UAM_ABI_VERSION the library itself was built with. */
unsigned uam_abi_version(void);

#define UAM_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* Recover the enclosing struct from a pointer to one of its members. */
#define UAM_CONTAINER_OF(ptr, type, member) \
    ((type *)(void *)((char *)(ptr) - offsetof(type, member)))

/* Minimal logging used by all modules; writes to stderr. */
typedef enum { UAM_LOG_ERROR, UAM_LOG_WARN, UAM_LOG_INFO, UAM_LOG_DEBUG } uam_log_level;
extern uam_log_level uam_log_threshold;
void uam_log(uam_log_level lvl, const char *module, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#endif /* UAM_COMMON_H */
