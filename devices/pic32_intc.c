/*
 * pic32_intc.c - PIC32-style vectored interrupt controller (uam/dev/pic32_intc.h).
 *
 * The controller recomputes its request after every change of a flag, enable,
 * priority, offset or mode bit, and tells the processor (the EIC callback) only
 * when the request changes. There is no acknowledge: the processor raises
 * Status.IPL in the handler, and software clears the flag.
 */
#include <stdlib.h>
#include <string.h>

#include "uam/bus.h"
#include "uam/dev/pic32_intc.h"

#define NREG_MAX (UAM_PIC32_INTC_MAX_IRQS / 32)
#define NO_OFFSET 0xFFFFFFFFu   /* UAM_MIPS_EIC_NO_OFFSET */

#define INTCON_MVEC (1u << 12)
#define INTCON_SS0  (1u << 16)

typedef struct pic32_intc {
    uam_device dev;
    uam_pic32_intc_config cfg;
    uint8_t    vec_of[UAM_PIC32_INTC_MAX_IRQS];
    unsigned   nreg;                       /* IFS/IEC registers */
    uint32_t   intcon, priss, iptmr;
    uint32_t   ifs[NREG_MAX], iec[NREG_MAX], level[NREG_MAX];
    uint8_t    ipc[UAM_PIC32_INTC_MAX_IRQS];   /* [4:2] IP, [1:0] IS per vector */
    uint32_t   off[UAM_PIC32_INTC_MAX_IRQS];
    /* the request presented */
    uint32_t   ripl, vector, offset, ss, irq;
    bool       presented;
    uam_pic32_intc_eic_fn eic;
    void      *eic_ctx;
} pic32_intc;

static pic32_intc *to_intc(void *d) { return UAM_CONTAINER_OF((uam_device *)d, pic32_intc, dev); }

static unsigned n_vectors(const pic32_intc *c)
{
    return c->cfg.variable_offset ? c->cfg.n_irqs : c->cfg.n_vectors;
}

/* Register offsets of the two layouts. */
static uint32_t off_ifs(const pic32_intc *c) { return c->cfg.variable_offset ? 0x040u : 0x030u; }
static uint32_t off_iec(const pic32_intc *c)
{
    return c->cfg.variable_offset ? 0x0C0u : off_ifs(c) + 0x10u * c->nreg;
}
static uint32_t off_ipc(const pic32_intc *c)
{
    return c->cfg.variable_offset ? 0x140u : off_iec(c) + 0x10u * c->nreg;
}
#define OFF_OFFX 0x540u

static void recompute(pic32_intc *c)
{
    unsigned best_irq = 0, best_vec = 0, best_pri = 0, best_sub = 0;
    bool found = false;
    for (unsigned r = 0; r < c->nreg; r++) {
        uint32_t act = c->ifs[r] & c->iec[r];
        while (act) {
            unsigned bit = (unsigned)__builtin_ctz(act);
            act &= act - 1;
            unsigned irq = r * 32 + bit, vec = c->vec_of[irq];
            unsigned pri = (c->ipc[vec] >> 2) & 7, sub = c->ipc[vec] & 3;
            if (!pri)
                continue;
            if (!found || pri > best_pri || (pri == best_pri && sub > best_sub)
                || (pri == best_pri && sub == best_sub && vec < best_vec)) {
                found = true;
                best_irq = irq;
                best_vec = vec;
                best_pri = pri;
                best_sub = sub;
            }
        }
    }
    bool mvec = c->intcon & INTCON_MVEC;
    uint32_t ripl = found ? best_pri : 0, vector = 0, offset = NO_OFFSET, ss = 0;
    if (found) {
        if (mvec) {
            vector = best_vec;
            if (c->cfg.variable_offset) {
                offset = c->off[best_vec] & 0x3FFFEu;
                ss = (c->priss >> (4 * best_pri)) & 0xFu;
            } else {
                ss = c->cfg.srs_priority && best_pri == c->cfg.srs_priority ? 1 : 0;
            }
        } else {
            ss = c->cfg.variable_offset ? (c->priss & 1u) : (c->intcon & INTCON_SS0) ? 1u : 0u;
        }
    }
    c->irq = best_irq;
    if (c->presented && ripl == c->ripl && vector == c->vector && offset == c->offset && ss == c->ss)
        return;
    c->presented = true;
    c->ripl = ripl;
    c->vector = vector;
    c->offset = offset;
    c->ss = ss;
    if (c->eic)
        c->eic(c->eic_ctx, ripl, vector, offset, ss);
}

void uam_pic32_intc_input(void *intc, unsigned irq, int level)
{
    pic32_intc *c = to_intc(intc);
    if (irq >= c->cfg.n_irqs)
        return;
    uint32_t m = 1u << (irq % 32);
    if (level) {
        c->level[irq / 32] |= m;
        c->ifs[irq / 32] |= m;
    } else {
        c->level[irq / 32] &= ~m;
    }
    recompute(c);
}

void uam_pic32_intc_set_eic(uam_device *dev, uam_pic32_intc_eic_fn fn, void *ctx)
{
    pic32_intc *c = to_intc(dev);
    c->eic = fn;
    c->eic_ctx = ctx;
    c->presented = false;
    recompute(c);
}

/* The value of the register at `reg` (a 16-byte aligned offset), and a pointer
 * to it for writes (NULL if read-only or absent). */
static uint32_t *reg_ptr(pic32_intc *c, uint32_t reg, uint32_t *val, uint32_t *wmask)
{
    *val = 0;
    *wmask = 0;
    bool vo = c->cfg.variable_offset;
    if (reg == 0x000) {
        *val = c->intcon;
        *wmask = INTCON_MVEC | (7u << 8) | 0x1Fu | (vo ? 0 : INTCON_SS0);
        return &c->intcon;
    }
    if (vo && reg == 0x010) {
        *val = c->priss;
        *wmask = 0xFFFFFFF1u;
        return &c->priss;
    }
    if (reg == (vo ? 0x020u : 0x010u)) {     /* INTSTAT */
        *val = c->ripl ? (c->ripl << 8) | (vo ? (c->irq & 0xFFu) : (c->vector & 0x3Fu)) : 0;
        return NULL;
    }
    if (reg == (vo ? 0x030u : 0x020u)) {
        *val = c->iptmr;
        *wmask = 0xFFFFFFFFu;
        return &c->iptmr;
    }
    if (reg >= off_ifs(c) && reg < off_ifs(c) + 0x10u * c->nreg) {
        unsigned k = (reg - off_ifs(c)) / 0x10u;
        *val = c->ifs[k];
        *wmask = 0xFFFFFFFFu;
        return &c->ifs[k];
    }
    if (reg >= off_iec(c) && reg < off_iec(c) + 0x10u * c->nreg) {
        unsigned k = (reg - off_iec(c)) / 0x10u;
        *val = c->iec[k];
        *wmask = 0xFFFFFFFFu;
        return &c->iec[k];
    }
    return NULL;
}

static unsigned ipc_regs(const pic32_intc *c) { return (n_vectors(c) + 3) / 4; }

static uam_status intc_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    pic32_intc *c = to_intc(d);
    uint32_t v = 0, wm;
    (void)size;
    if (c->cfg.variable_offset && off >= OFF_OFFX) {
        uint64_t n = (off - OFF_OFFX) / 4;
        v = n < n_vectors(c) ? c->off[n] : 0;
    } else if (off >= off_ipc(c) && off < off_ipc(c) + 0x10u * ipc_regs(c)) {
        if ((off & 0xF) == 0) {
            unsigned n = (unsigned)(off - off_ipc(c)) / 0x10u * 4;
            for (unsigned k = 0; k < 4 && n + k < n_vectors(c); k++)
                v |= (uint32_t)c->ipc[n + k] << (8 * k);
        }
    } else if ((off & 0xF) == 0) {
        reg_ptr(c, (uint32_t)off, &v, &wm);
    }
    *val = v >> (8 * (off & 3));
    return UAM_OK;
}

static uint32_t apply(uint32_t old, uint32_t v, unsigned alias)
{
    switch (alias) {
    case 0x0: return v;
    case 0x4: return old & ~v;   /* CLR */
    case 0x8: return old | v;    /* SET */
    default:  return old ^ v;    /* INV */
    }
}

static uam_status intc_write(uam_device *d, uint64_t off, unsigned size, uint64_t v64)
{
    pic32_intc *c = to_intc(d);
    uint32_t v = (uint32_t)v64;
    if (size < 4)
        v <<= 8 * (off & 3);   /* a byte or halfword store lands in its lane */
    uint32_t reg = (uint32_t)off & ~0xFu;
    unsigned alias = (unsigned)off & 0xCu;
    if (c->cfg.variable_offset && off >= OFF_OFFX) {
        uint64_t n = (off - OFF_OFFX) / 4;
        if (n < n_vectors(c))
            c->off[n] = v & 0x3FFFEu;
    } else if (reg >= off_ipc(c) && reg < off_ipc(c) + 0x10u * ipc_regs(c)) {
        unsigned n = (reg - off_ipc(c)) / 0x10u * 4;
        uint32_t old = 0;
        for (unsigned k = 0; k < 4 && n + k < n_vectors(c); k++)
            old |= (uint32_t)c->ipc[n + k] << (8 * k);
        uint32_t nv = apply(old, v, alias) & 0x1F1F1F1Fu;
        for (unsigned k = 0; k < 4 && n + k < n_vectors(c); k++)
            c->ipc[n + k] = (uint8_t)(nv >> (8 * k));
    } else {
        uint32_t cur, wm;
        uint32_t *p = reg_ptr(c, reg, &cur, &wm);
        if (p)
            *p = (cur & ~wm) | (apply(cur, v, alias) & wm);
        /* a source still requesting keeps its flag */
        for (unsigned k = 0; k < c->nreg; k++)
            c->ifs[k] |= c->level[k];
    }
    recompute(c);
    return UAM_OK;
}

static uam_status intc_peek(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    return intc_read(d, off, size, val);
}

static void intc_reset(uam_device *d)
{
    pic32_intc *c = to_intc(d);
    c->intcon = c->priss = c->iptmr = 0;
    memset(c->iec, 0, sizeof(c->iec));
    memset(c->ipc, 0, sizeof(c->ipc));
    memset(c->off, 0, sizeof(c->off));
    memcpy(c->ifs, c->level, sizeof(c->ifs));   /* inputs still high request again */
    c->presented = false;
    recompute(c);
}

static void intc_destroy(uam_device *d) { free(to_intc(d)); }

static void intc_save(uam_device *d, uam_state_writer *w)
{
    pic32_intc *c = to_intc(d);
    uam_state_put_u32(w, c->cfg.n_irqs);
    uam_state_put_u32(w, c->intcon);
    uam_state_put_u32(w, c->priss);
    uam_state_put_u32(w, c->iptmr);
    for (unsigned k = 0; k < c->nreg; k++) {
        uam_state_put_u32(w, c->ifs[k]);
        uam_state_put_u32(w, c->iec[k]);
        uam_state_put_u32(w, c->level[k]);
    }
    for (unsigned n = 0; n < n_vectors(c); n++) {
        uam_state_put_u8(w, c->ipc[n]);
        uam_state_put_u32(w, c->off[n]);
    }
    uam_state_put_u32(w, c->ripl);
    uam_state_put_u32(w, c->vector);
    uam_state_put_u32(w, c->offset);
    uam_state_put_u32(w, c->ss);
    uam_state_put_u32(w, c->irq);
    uam_state_put_bool(w, c->presented);
}

static void intc_restore(uam_device *d, uam_state_reader *r)
{
    pic32_intc *c = to_intc(d);
    uam_state_expect_u32(r, c->cfg.n_irqs, "PIC32 interrupt controller sources");
    if (!uam_state_ok(r))
        return;
    c->intcon = uam_state_get_u32(r);
    c->priss = uam_state_get_u32(r);
    c->iptmr = uam_state_get_u32(r);
    for (unsigned k = 0; k < c->nreg; k++) {
        c->ifs[k] = uam_state_get_u32(r);
        c->iec[k] = uam_state_get_u32(r);
        c->level[k] = uam_state_get_u32(r);
    }
    for (unsigned n = 0; n < n_vectors(c); n++) {
        c->ipc[n] = uam_state_get_u8(r);
        c->off[n] = uam_state_get_u32(r);
    }
    /* What the processor was shown; it restores its own copy. */
    c->ripl = uam_state_get_u32(r);
    c->vector = uam_state_get_u32(r);
    c->offset = uam_state_get_u32(r);
    c->ss = uam_state_get_u32(r);
    c->irq = uam_state_get_u32(r);
    c->presented = uam_state_get_bool(r);
}

static const uam_device_ops intc_ops = {
    .type = "pic32,intc",
    .endian = UAM_ACC_LE,
    .read = intc_read,
    .write = intc_write,
    .peek = intc_peek,
    .reset = intc_reset,
    .destroy = intc_destroy,
    .save = intc_save,
    .restore = intc_restore,
};

uam_device *uam_pic32_intc_create(const char *name, const uam_pic32_intc_config *cfg)
{
    if (!cfg || !cfg->n_irqs || cfg->n_irqs % 32 || cfg->n_irqs > UAM_PIC32_INTC_MAX_IRQS)
        return NULL;
    if (!cfg->variable_offset && (!cfg->n_vectors || cfg->n_vectors > 64))
        return NULL;
    pic32_intc *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    uam_device_init(&c->dev, &intc_ops, name);
    c->cfg = *cfg;
    c->cfg.irq_vector = NULL;   /* copied below */
    c->nreg = cfg->n_irqs / 32;
    for (unsigned i = 0; i < cfg->n_irqs; i++) {
        unsigned v = cfg->variable_offset ? i : cfg->irq_vector ? cfg->irq_vector[i] : i % cfg->n_vectors;
        if (v >= n_vectors(c)) {
            free(c);
            return NULL;
        }
        c->vec_of[i] = (uint8_t)v;
    }
    c->offset = NO_OFFSET;
    return &c->dev;
}
