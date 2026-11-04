/*
 * uam/dev/pic32_intc.h - PIC32-style vectored interrupt controller: per-source
 * flag (IFS) and enable (IEC) bits, a priority and subpriority per vector (IPC),
 * and the highest request presented to a MIPS processor's external interrupt
 * controller interface (uam_mips_eic_input).
 *
 * Every register is 32 bits on a 16-byte stride, with CLR, SET and INV aliases at
 * +4, +8 and +0xC that clear, set or invert the bits written as 1 (the OFF
 * registers of the variable-offset layout are plain words, 4 bytes apart).
 *
 * Computed-offset layout (PIC32MX), n_irqs sources and n_vectors vectors:
 *   0x000  INTCON   [16] SS0, [12] MVEC, [10:8] TPC, [4:0] INTxEP
 *   0x010  INTSTAT  [10:8] SRIPL, [5:0] VEC: the request presented (read-only)
 *   0x020  IPTMR
 *   0x030  IFS0..   one register per 32 sources, then IEC0.. likewise, then IPC0..
 *                   one per four vectors: byte k is [4:2] IP, [1:0] IS of vector 4n+k
 *   The vector of a source comes from irq_vector (NULL: the source number modulo
 *   n_vectors). The processor computes the address, EBase + 0x200 + vector *
 *   IntCtl.VS spacing; in multi-vector mode the priority level `srs_priority`
 *   uses shadow set 1.
 * Variable-offset layout (PIC32MZ, PIC32MM): one vector per source,
 *   0x000  INTCON   [12] MVEC, [10:8] TPC, [4:0] INTxEP
 *   0x010  PRISS    [31:4] PRIxSS: the shadow set of priority level x; [0] SS0
 *   0x020  INTSTAT  [10:8] SRIPL, [7:0] SIRQ
 *   0x030  IPTMR
 *   0x040  IFS0..IFS7, 0x0C0 IEC0..IEC7, 0x140 IPC0..IPC63 (per source),
 *   0x540  OFF000.. [17:1] the vector's offset from EBase (in multi-vector mode)
 * Unimplemented sources read as zero. In single-vector mode (MVEC = 0) every
 * request is presented as vector 0 (EBase + 0x200) with shadow set SS0.
 *
 * Inputs are levels: a high input sets its flag, and keeps it set while high
 * (a cleared flag of a source still requesting is set again at once). Software
 * may also set flags. The request goes to the highest priority, then subpriority,
 * then lowest vector (natural order) among enabled flagged sources of priority
 * above 0. The proximity timer (IPTMR, TPC) is not modelled: requests are never
 * held off.
 */
#ifndef UAM_DEV_PIC32_INTC_H
#define UAM_DEV_PIC32_INTC_H

#include "uam/device.h"

#define UAM_PIC32_INTC_SIZE     0x1000u
#define UAM_PIC32_INTC_MAX_IRQS 256u

typedef struct uam_pic32_intc_config {
    unsigned       n_irqs;           /* sources, a multiple of 32, up to 256 */
    unsigned       n_vectors;        /* computed layout: vectors (<= 64); else n_irqs */
    const uint8_t *irq_vector;       /* computed layout: source -> vector (NULL: modulo n_vectors) */
    bool           variable_offset;  /* the PIC32MZ/MM layout with PRISS and OFFx */
    unsigned       srs_priority;     /* computed layout: level given shadow set 1 (0: none) */
} uam_pic32_intc_config;

/* The request the controller presents, in uam_mips_eic_input's shape. */
typedef void (*uam_pic32_intc_eic_fn)(void *ctx, unsigned ripl, unsigned vector,
                                      uint32_t offset, unsigned shadow_set);

uam_device *uam_pic32_intc_create(const char *name, const uam_pic32_intc_config *cfg);
/* Where the request goes (connect to uam_mips_eic_input with the processor). */
void        uam_pic32_intc_set_eic(uam_device *dev, uam_pic32_intc_eic_fn fn, void *ctx);
/* Signal sink for source lines: uam_signal_connect(&irq, uam_pic32_intc_input, intc, n). */
void        uam_pic32_intc_input(void *intc, unsigned irq, int level);

#endif /* UAM_DEV_PIC32_INTC_H */
