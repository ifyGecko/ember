/*
 * uam/fuzz_i2s.h - Input-to-state: candidates from one run's compare log.
 *
 * A firmware that compares part of its input with a constant usually holds that part in
 * the input as is, in one byte order or the other. So for each compare the log (cmp.h)
 * recorded, these functions look for one operand's bytes in the input and write the other
 * operand there; for an ordered compare also the other operand plus and minus one, to land
 * on either side of a boundary. Both byte orders are tried, at the compare's width and at
 * the narrowest of 1, 2 and 4 bytes that holds both values (a byte compared in a 32-bit
 * register is one byte of input). A constant of the code (a record with UAM_CMP_IMM) is
 * never searched for: only its other operand is. Patterns of zero bytes are not searched
 * (they are everywhere), and of one pattern the first UAM_I2S_OCCURRENCES positions are
 * tried.
 *
 * A candidate is the input with one patch applied: `len` bytes at `pos`, where the patch
 * differs from the input in its first and its last byte (so two patches that give the same
 * bytes are the same patch). Candidates are distinct and none equals the input.
 *
 * Constants of the code with two or more significant bytes that the input holds in
 * neither byte order are returned as dictionary tokens, for compares made on a
 * transformed copy of the input.
 *
 * These are pure functions; the campaign engine (fuzz_campaign.h) runs the candidates.
 *
 * Dependencies: cmp.h.
 */
#ifndef UAM_FUZZ_I2S_H
#define UAM_FUZZ_I2S_H

#include "uam/cmp.h"

#define UAM_I2S_OCCURRENCES 8u   /* positions of one pattern tried */

typedef struct uam_i2s_patch {
    uint32_t pos;
    uint8_t  len;        /* 1..8 */
    uint8_t  bytes[8];
} uam_i2s_patch;

typedef struct uam_i2s_token {
    uint64_t value;
    uint8_t  width;      /* bytes: 2, 4 or 8 */
} uam_i2s_token;

/* The candidates of data[0..len) for the log's n records, in the order derived, at most
 * max: how many were written to out. */
size_t uam_i2s_candidates(const uint8_t *data, size_t len, const uam_cmp_entry *log, size_t n,
                          uam_i2s_patch *out, size_t max);

/* The log's constants that data does not hold, each once, at most max. */
size_t uam_i2s_tokens(const uint8_t *data, size_t len, const uam_cmp_entry *log, size_t n,
                      uam_i2s_token *out, size_t max);

#endif /* UAM_FUZZ_I2S_H */
