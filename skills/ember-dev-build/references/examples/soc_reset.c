/*
 * soc_reset.c - <SoC> reset/clock block @ phys 0x........ Driver evidence:
 *   sysReboot 0x........: RST_CTRL = RST_FULL_CHIP, then spins forever
 *   pllInit   0x........: writes PLL_CFG, polls PLL_CFG & PLL_LOCK
 *   sysClkGet 0x........: divides by ((CLK_CTRL >> 2) & 3) + 1 -> must not trap
 */
#include <stdlib.h>

#include "soc_reset.h"

#define R_PLL_CFG  0x00u
#define  PLL_LOCK       (1u << 31)     /* reads set: the PLL locks instantly */
#define  PLL_BYPASS     (1u << 16)     /* self-clearing: "apply" completes at once */
#define R_CLK_CTRL 0x08u
#define R_RST_CTRL 0x1cu
#define  RST_FULL_CHIP  (1u << 24)

typedef struct soc_reset {
    uam_device dev;
    uam_signal reset;
    uint32_t   pll_cfg, clk_ctrl, rst_ctrl;
} soc_reset;

static soc_reset *to_rst(uam_device *d) { return UAM_CONTAINER_OF(d, soc_reset, dev); }

static uam_status rst_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    soc_reset *r = to_rst(d);
    (void)size;
    switch (off) {
    case R_PLL_CFG:  *val = r->pll_cfg | PLL_LOCK; break;
    case R_CLK_CTRL: *val = r->clk_ctrl; break;
    case R_RST_CTRL: *val = r->rst_ctrl; break;
    default:         *val = 0; break;
    }
    return UAM_OK;
}

static uam_status rst_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    soc_reset *r = to_rst(d);
    (void)size;
    switch (off) {
    case R_PLL_CFG:  r->pll_cfg = (uint32_t)v & ~(PLL_LOCK | PLL_BYPASS); break;
    case R_CLK_CTRL: r->clk_ctrl = (uint32_t)v; break;
    case R_RST_CTRL:
        r->rst_ctrl = (uint32_t)v;
        if (v & RST_FULL_CHIP)
            uam_signal_set(&r->reset, 1);   /* reset happens at the next instruction boundary */
        break;
    default: break;
    }
    return UAM_OK;
}

static void rst_reset(uam_device *d)
{
    soc_reset *r = to_rst(d);
    r->pll_cfg = 0;
    r->clk_ctrl = 0x5u << 2;     /* non-zero divider field: sysClkGet divides by it */
    r->rst_ctrl = 0;
    uam_signal_set(&r->reset, 0);    /* lower the line again (the machine is resetting us) */
}

static void rst_destroy(uam_device *d) { free(to_rst(d)); }

static void rst_save(uam_device *d, uam_state_writer *w)
{
    soc_reset *r = to_rst(d);
    uam_state_put_u32(w, r->pll_cfg);
    uam_state_put_u32(w, r->clk_ctrl);
    uam_state_put_u32(w, r->rst_ctrl);
    uam_state_put_signal(w, &r->reset);
}

static void rst_restore(uam_device *d, uam_state_reader *r)
{
    soc_reset *s = to_rst(d);
    s->pll_cfg = uam_state_get_u32(r);
    s->clk_ctrl = uam_state_get_u32(r);
    s->rst_ctrl = uam_state_get_u32(r);
    uam_state_get_signal(r, &s->reset);
}

static const uam_device_ops soc_reset_ops = {
    .type = "soc-reset",
    .endian = UAM_ACC_BE,
    .impl_min = 4, .impl_max = 4,
    .read = rst_read,
    .write = rst_write,
    .reset = rst_reset,
    .destroy = rst_destroy,
    .save = rst_save,
    .restore = rst_restore,
};

uam_device *soc_reset_create(const char *name)
{
    soc_reset *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    uam_device_init(&r->dev, &soc_reset_ops, name);
    uam_signal_init(&r->reset);
    return &r->dev;
}

uam_signal *soc_reset_line(uam_device *d) { return &to_rst(d)->reset; }
