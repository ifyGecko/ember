/*
 * uam/dev/acpi_pm.h - ACPI fixed-hardware power-management registers:
 * PM1 event (status/enable), PM1 control, the PM timer and a GPE0 block, in
 * one I/O window with a layout the platform chooses (ACPI 6, chapter 4.8).
 *
 *   PM1_STS/PM1_EN   TMR (0), BM (4), GBL (5), PWRBTN (8), SLPBTN (9), RTC (10)
 *                    and WAK (15, status only); status bits are write-1-to-clear
 *   PM1_CNT          SCI_EN (0), BM_RLD (1), GBL_RLS (2), SLP_TYP (10-12),
 *                    SLP_EN (13, write-only)
 *   PM_TMR           3.579545 MHz, 24 or 32 bits; TMR_STS sets when its top bit
 *                    toggles
 *   GPE0             status half (write-1-to-clear) then enable half
 *
 * The SCI line is high while SCI_EN is set and an enabled status bit is set.
 * Writing SLP_EN calls the `sleep` hook with SLP_TYP: what each value means is
 * the platform's (its DSDT's \_Sx objects). Offsets in the window that are
 * none of these registers go to the `read`/`write` hooks, so a chipset model
 * keeps its own registers (SMI control, TCO, ...) in the same window.
 */
#ifndef UAM_DEV_ACPI_PM_H
#define UAM_DEV_ACPI_PM_H

#include "uam/device.h"
#include "uam/sched.h"
#include "uam/signal.h"

#define UAM_ACPI_PM_TMR_HZ  3579545u
#define UAM_ACPI_PM_ABSENT  UINT32_MAX

enum {
    UAM_ACPI_PM1_TMR = 1u << 0, UAM_ACPI_PM1_BM = 1u << 4, UAM_ACPI_PM1_GBL = 1u << 5,
    UAM_ACPI_PM1_PWRBTN = 1u << 8, UAM_ACPI_PM1_SLPBTN = 1u << 9, UAM_ACPI_PM1_RTC = 1u << 10,
    UAM_ACPI_PM1_WAK = 1u << 15,
};

typedef struct uam_acpi_pm_layout {
    uint32_t size;       /* the whole window, bytes */
    uint32_t pm1_evt;    /* offsets of each block, or UAM_ACPI_PM_ABSENT */
    uint32_t pm1_cnt;
    uint32_t pm_tmr;
    uint32_t gpe0;
    uint32_t gpe0_len;   /* bytes: status half + enable half (even, <= 16) */
    bool     tmr_32bit;  /* TMR_VAL_EXT */
} uam_acpi_pm_layout;

typedef struct uam_acpi_pm_hooks {
    void *ctx;
    /* SLP_EN was written with this SLP_TYP. */
    void       (*sleep)(void *ctx, unsigned slp_typ);
    /* Window offsets that are no ACPI register (optional; else RAZ/WI). */
    uam_status (*read)(void *ctx, uint64_t off, unsigned size, uint64_t *val);
    uam_status (*write)(void *ctx, uint64_t off, unsigned size, uint64_t val);
} uam_acpi_pm_hooks;

uam_device *uam_acpi_pm_create(const char *name, uam_sched *sched, uint64_t tick_hz,
                               const uam_acpi_pm_layout *layout, const uam_acpi_pm_hooks *hooks);
uam_signal *uam_acpi_pm_sci_line(uam_device *dev);
/* Set PM1 status bits (power button, RTC alarm, wake). */
void        uam_acpi_pm_event(uam_device *dev, uint16_t pm1_sts);
/* Set a GPE0 status bit (a general-purpose event; signal sink: input = bit). */
void        uam_acpi_pm_gpe(void *dev, unsigned bit, int level);
/* The timer value (for chipset models that mirror it elsewhere). */
uint32_t    uam_acpi_pm_timer(uam_device *dev);

#endif /* UAM_DEV_ACPI_PM_H */
