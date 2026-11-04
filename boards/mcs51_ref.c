/*
 * mcs51_ref.c - "mcs51-ref": the 8051 reference board. Program memory, external data
 * RAM, the 8051's own timer block, serial port and ports in the SFR space, and a
 * simulation finisher; no other peripheral of any part.
 *
 * -o model= picks the core (uam/isa/mcs51.h):
 *
 *   model  core                     program memory           SFR paging
 *   8051   uam_mcs51_8051_ref       64 KiB                   -
 *   8052   uam_mcs51_8052_ref       64 KiB (default)         -
 *   cip51  uam_mcs51_cip51_ref      128 KiB, banked from     SFRPAGE (0x84); the
 *                                   0x8000 (PSBANK at 0xB1:  devices are on page 0,
 *                                   IFBANK 1:0, COBANK 5:4)  P0-P3, PSBANK and EMI0CN
 *                                                            on pages 0-15, P4-P7
 *                                                            (0xC8-0xF8) on page 0x0F
 *
 * SFRs (uam/dev/mcs51_timer.h, mcs51_serial.h, mcs51_port.h):
 *   0x80 P0, 0x90 P1, 0xA0 P2, 0xB0 P3   ports (P2 also pages MOVX @Ri on 8051/8052)
 *   0x88-0x8D TCON TMOD TL0 TL1 TH0 TH1  Timer 0/1: sources 0 IE0, 1 TF0, 2 IE1, 3 TF1
 *   0x98-0x99 SCON SBUF                  serial port, the console: source 4
 *   0xAA EMI0CN (cip51)                  MOVX @Ri's page
 * External data: RAM at 0x0000-0xFEFF (--ram sets its size), and at 0xFF00 the test
 * finisher (byte commands: uam/dev/test_finisher.h). An 8051 has one processor:
 * -o cpus is 1.
 */
#include "uam/boards.h"
#include <stdlib.h>
#include <string.h>

#include "uam/dev/mcs51_port.h"
#include "uam/dev/mcs51_serial.h"
#include "uam/dev/mcs51_timer.h"
#include "uam/dev/test_finisher.h"
#include "uam/dev/unknown_mmio.h"
#include "uam/isa/mcs51.h"
#include "uam/machine.h"

#define SERIAL_SOURCE 4u
#define XDATA_DEV     0xFF00u

typedef struct mcs51_model {
    const char             *name;
    const uam_mcs51_config *cfg;
    uint32_t                code_size;
} mcs51_model;

static const mcs51_model models[] = {
    { "8051",  &uam_mcs51_8051_ref,  64u << 10 },
    { "8052",  &uam_mcs51_8052_ref,  64u << 10 },
    { "cip51", &uam_mcs51_cip51_ref, 128u << 10 },
};

static void irq_ack(void *timer, unsigned source)
{
    if (source < 4)
        uam_mcs51_timer_ack(timer, source);
}

static void psbank(void *cpu, uint8_t v)
{
    uam_mcs51_set_code_bank(cpu, v & 3u, (v >> 4) & 3u);
}

static uam_status serial_input(void *ctx, uam_machine *m, const uint8_t *data, size_t len)
{
    (void)m;
    uam_mcs51_serial_feed(ctx, data, len);   /* the excess is dropped, as on a real line */
    return UAM_OK;
}

/* Map a device at SFR `sfr` on pages first..first+pages-1 (page 0 alone without
 * paging). */
static uam_status map_sfr(uam_machine *m, const char *name, unsigned sfr, uint64_t size, uam_device *d,
                          unsigned first, unsigned pages)
{
    uam_status st = UAM_OK;
    for (unsigned p = first; p < first + pages && st == UAM_OK; p++)
        st = uam_bus_map_mmio(&m->bus, name, UAM_MCS51_SFR_BASE + p * 0x100u + sfr, size, d);
    return st;
}

static uam_status add(uam_machine *m, uam_device *d)
{
    if (!d)
        return UAM_ERR_NOMEM;
    uam_machine_add_device(m, d);
    return UAM_OK;
}

static uam_status mcs51_ref_build(uam_machine *m, const uam_board_config *cfg)
{
    const char *mname = uam_board_config_option(cfg, "model");
    const mcs51_model *md = NULL;
    for (size_t i = 0; i < UAM_ARRAY_LEN(models); i++)
        if (!mname ? strcmp(models[i].name, "8052") == 0 : strcmp(models[i].name, mname) == 0)
            md = &models[i];
    if (!md) {
        uam_log(UAM_LOG_ERROR, "mcs51-ref", "model=%s: 8051, 8052 or cip51", mname);
        return UAM_ERR_INVALID;
    }
    const char *ncpu = uam_board_config_option(cfg, "cpus");
    if (ncpu && strtoul(ncpu, NULL, 0) != 1) {
        uam_log(UAM_LOG_ERROR, "mcs51-ref", "cpus=%s: an 8051 has one processor", ncpu);
        return UAM_ERR_INVALID;
    }
    if (cfg->ram_size > XDATA_DEV) {
        uam_log(UAM_LOG_ERROR, "mcs51-ref", "--ram: at most 0x%x bytes of external data RAM", XDATA_DEV);
        return UAM_ERR_INVALID;
    }

    uam_status st = uam_machine_init(m, &uam_isa_mcs51);
    if (st != UAM_OK)
        return st;
    m->board_name = "mcs51-ref";
    m->reset_pc = 0;
    if ((st = uam_mcs51_configure(&m->cpu, md->cfg)) != UAM_OK)
        return st;

    /* Program memory, erased (0xFF) until the image is loaded. */
    uint8_t *erased = malloc(md->code_size);
    if (!erased)
        return UAM_ERR_NOMEM;
    memset(erased, 0xFF, md->code_size);
    st = uam_bus_map_rom(&m->bus, "code", 0, md->code_size, erased, md->code_size);
    free(erased);
    if (st != UAM_OK)
        return st;
    if ((st = uam_bus_map_ram(&m->bus, "xram", UAM_MCS51_XDATA_BASE, cfg->ram_size ? cfg->ram_size : XDATA_DEV))
        != UAM_OK)
        return st;

    const bool cip = md->cfg->sfrpage != 0;
    const unsigned all_pages = cip ? 16 : 1;
    uam_device *timer = uam_mcs51_timer_create("timer", &m->sched);
    uam_device *serial = uam_mcs51_serial_create("serial0", cfg->console);
    uam_device *test = uam_test_finisher_create("test", uam_machine_stop_cb, m);
    if ((st = add(m, timer)) != UAM_OK || (st = add(m, serial)) != UAM_OK || (st = add(m, test)) != UAM_OK)
        return st;
    if ((st = map_sfr(m, "timer", UAM_MCS51_SFR_TCON, UAM_MCS51_TIMER_SIZE, timer, 0, 1)) != UAM_OK
        || (st = map_sfr(m, "serial0", UAM_MCS51_SFR_SCON, UAM_MCS51_SERIAL_SIZE, serial, 0, 1)) != UAM_OK
        || (st = uam_bus_map_mmio(&m->bus, "test", UAM_MCS51_XDATA_BASE + XDATA_DEV, 0x10, test)) != UAM_OK)
        return st;
    static const char *const port_names[4] = { "p0", "p1", "p2", "p3" };
    for (unsigned i = 0; i < 4; i++) {
        uam_device *p = uam_mcs51_port_create(port_names[i]);
        if ((st = add(m, p)) != UAM_OK
            || (st = map_sfr(m, port_names[i], UAM_MCS51_SFR_P0 + 0x10u * i, 1, p, 0, all_pages)) != UAM_OK)
            return st;
    }
    if (cip) {
        uam_device *bank = uam_mcs51_reg_create("psbank", 0x11, psbank, &m->cpu);
        uam_device *emi = uam_mcs51_reg_create("emi0cn", 0x00, NULL, NULL);
        if ((st = add(m, bank)) != UAM_OK || (st = add(m, emi)) != UAM_OK
            || (st = map_sfr(m, "psbank", 0xB1, 1, bank, 0, all_pages)) != UAM_OK
            || (st = map_sfr(m, "emi0cn", md->cfg->xpage, 1, emi, 0, all_pages)) != UAM_OK)
            return st;
        static const char *const hi_ports[4] = { "p4", "p5", "p6", "p7" };
        for (unsigned i = 0; i < 4; i++) {
            uam_device *p = uam_mcs51_port_create(hi_ports[i]);
            if ((st = add(m, p)) != UAM_OK
                || (st = map_sfr(m, hi_ports[i], 0xC8 + 0x10u * i, 1, p, 0x0F, 1)) != UAM_OK)
                return st;
        }
    }

    for (unsigned k = 0; k < 4; k++)
        uam_signal_connect(uam_mcs51_timer_irq(timer, k), uam_cpu_irq_sink, &m->cpu, k);
    uam_signal_connect(uam_mcs51_serial_irq(serial), uam_cpu_irq_sink, &m->cpu, SERIAL_SOURCE);
    uam_mcs51_set_irq_ack(&m->cpu, irq_ack, timer);
    if ((st = uam_machine_add_input(m, "uart0", serial_input, serial)) != UAM_OK)
        return st;

    if (cfg->unknown_mmio != UAM_UNKNOWN_FAULT) {
        uam_device *um = uam_unknown_mmio_create(
            "unknown-mmio", cfg->unknown_mmio == UAM_UNKNOWN_ECHO ? UAM_UNKNOWN_POLICY_ECHO
                                                                  : UAM_UNKNOWN_POLICY_ZERO);
        if (!um)
            return UAM_ERR_NOMEM;
        uam_machine_add_device(m, um);
        uam_bus_set_unmapped(&m->bus, um);
    }

    uam_machine_reset(m, m->reset_pc);
    return UAM_OK;
}

const uam_board uam_board_mcs51_ref = {
    .name = "mcs51-ref",
    .description = "8051 reference board (-o model=8051|8052|cip51; timers, serial port, ports, test finisher)",
    .build = mcs51_ref_build,
};
