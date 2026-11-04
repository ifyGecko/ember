/*
 * uam/isa/mcs51.h - Public face of the 8051 (MCS-51) ISA module.
 *
 * One descriptor, uam_isa_mcs51, for every core that runs the MCS-51 instruction set: the
 * Intel 8051 and 8052 and the many derivatives built on them, classic (12 clocks per
 * machine cycle) or pipelined (Silicon Labs' CIP-51 and the like). They share the
 * instruction set, the four address spaces, the register banks and a handful of special
 * function registers (SFRs) at fixed addresses; they differ in the size of the internal
 * RAM, the interrupt system, the number of data pointers, how MOVX @Ri forms its address,
 * SFR paging and code banking. Those are the CPU model, uam_mcs51_config, which the
 * board selects with uam_mcs51_configure() after uam_machine_init() and before the first
 * uam_machine_reset() (without that call the hart is uam_mcs51_8051_ref). Every other
 * SFR - timers, serial ports, I/O ports, CAN, ADCs - is a peripheral: a device the board
 * maps in the SFR space.
 *
 * Memory. ember puts the spaces on the one bus:
 *   code (program memory)   bus 0x000000 + address (with code banking: see below)
 *   external data (MOVX)    UAM_MCS51_XDATA_BASE + address (16 bits)
 *   SFRs (0x80-0xFF)        UAM_MCS51_SFR_BASE + address; with SFR paging,
 *                           UAM_MCS51_SFR_BASE + page * 0x100 + address
 * The internal RAM (128 or 256 bytes; R0-R7 of the four banks, the bit-addressable
 * area, the stack) is part of the hart, like a register file: it is not on the bus.
 * The SFRs the core owns are ACC, B, PSW, SP, DPL/DPH, IE, IP, PCON and the model's
 * IPH, extended interrupt registers, data-pointer select and SFR page register; they
 * are the same on every SFR page. A debugger or loader uses bus addresses.
 *
 * Code banking. When bank_from is set, code addresses from bank_from up are a window
 * onto block n of a larger program memory, blocks being the window's size W = 0x10000 -
 * bank_from: bus address = n * W + (address - bank_from). Block bank_from / W is the
 * window's own addresses (the bank after reset); lower blocks alias the common area.
 * So a linear image is laid out as the part's flash is, and bank_from must be a
 * multiple of W (0x8000, 0xC000, ...). Instruction fetch and MOVC each have a bank, set
 * by the board's bank-select register through uam_mcs51_set_code_bank; a change takes
 * effect at the next block.
 *
 * Interrupts. Input line n is interrupt source n, whose vector is 0x0003 + 8n, as on
 * every 8051. Its enable bit is bit n of IE for n < 7 and bit n - 7 - 8k of the k-th
 * extended enable register above that (eie), its priority likewise in IP and eip (and,
 * for four levels, iph and eiph). EA (IE.7) gates all. A source is taken when its level
 * is above every level in progress; among equal levels the lowest source number wins
 * (the 8051's polling order). RETI ends the highest level in progress. Peripherals that
 * clear their flag when the vector is taken (timer overflow, edge-triggered external
 * interrupts) are told through uam_mcs51_set_irq_ack. Line 0 is source 0 (INT0).
 *
 * Time. Every instruction is one tick of virtual time, whatever its cycle count on the
 * part: timers count instructions.
 */
#ifndef UAM_ISA_MCS51_H
#define UAM_ISA_MCS51_H

#include "uam/isa.h"

extern const uam_isa uam_isa_mcs51;

#define UAM_MCS51_XDATA_BASE UINT64_C(0x1000000)
#define UAM_MCS51_SFR_BASE   UINT64_C(0x2000000)

/* The SFRs every 8051 has at these addresses. */
enum {
    UAM_MCS51_SFR_P0 = 0x80, UAM_MCS51_SFR_SP = 0x81, UAM_MCS51_SFR_DPL = 0x82,
    UAM_MCS51_SFR_DPH = 0x83, UAM_MCS51_SFR_PCON = 0x87, UAM_MCS51_SFR_TCON = 0x88,
    UAM_MCS51_SFR_P1 = 0x90, UAM_MCS51_SFR_SCON = 0x98, UAM_MCS51_SFR_P2 = 0xA0,
    UAM_MCS51_SFR_IE = 0xA8, UAM_MCS51_SFR_P3 = 0xB0, UAM_MCS51_SFR_IP = 0xB8,
    UAM_MCS51_SFR_PSW = 0xD0, UAM_MCS51_SFR_ACC = 0xE0, UAM_MCS51_SFR_B = 0xF0,
};

#define UAM_MCS51_SOURCES 32u

typedef struct uam_mcs51_config {
    const char *name;
    uint16_t    iram_size;   /* 128 (8051) or 256 (8052 and later) */
    uint8_t     n_sources;   /* interrupt sources, 1..UAM_MCS51_SOURCES */
    /* SFR addresses of the model's optional core registers; 0 = absent. */
    uint8_t     iph;         /* IP high bits: four priority levels (with eiph) */
    uint8_t     eie[3];      /* enables of sources 7-14, 15-22, 23-30 */
    uint8_t     eip[3];      /* their priorities */
    uint8_t     eiph[3];     /* their priority high bits */
    uint8_t     dps;         /* data-pointer select (bit 0): two DPTRs */
    uint8_t     dpl1;        /* with dps: DPTR1 at dpl1/dpl1+1 (else DPL/DPH switch) */
    uint8_t     xpage;       /* SFR whose value is MOVX @Ri's high address byte (P2 on
                              * classic parts); 0: the high byte is 0 */
    uint8_t     sfrpage;     /* SFR page register: SFR paging */
    uint16_t    bank_from;   /* code banking window start (0: no banking), a
                              * multiple of 0x10000 - bank_from */
} uam_mcs51_config;

/* Reference models naming no part. */
extern const uam_mcs51_config uam_mcs51_8051_ref;   /* 128 B RAM, 5 sources, 2 levels, P2 page */
extern const uam_mcs51_config uam_mcs51_8052_ref;   /* 256 B RAM, 6 sources, IPH (4 levels),
                                                     * two DPTRs (AUXR1 at 0xA2), P2 page */
extern const uam_mcs51_config uam_mcs51_cip51_ref;  /* 256 B RAM, 23 sources (EIE1/EIE2 at
                                                     * 0xE6/0xE7, EIP1/EIP2 at 0xF6/0xF7),
                                                     * 2 levels, EMI0CN-style page at 0xAA,
                                                     * SFR paging (SFRPAGE at 0x84), code
                                                     * banking from 0x8000 */

/* Select the CPU model of an 8051 hart. Takes effect at the next reset. Returns
 * UAM_ERR_INVALID if cpu is not an 8051 hart or cfg is malformed. */
uam_status uam_mcs51_configure(uam_cpu *cpu, const uam_mcs51_config *cfg);
/* The model in effect, or NULL if cpu is not an 8051 hart. */
const uam_mcs51_config *uam_mcs51_config_of(uam_cpu *cpu);

/* Called with each source the core takes. */
typedef void (*uam_mcs51_irq_ack_fn)(void *ctx, unsigned source);
void uam_mcs51_set_irq_ack(uam_cpu *cpu, uam_mcs51_irq_ack_fn fn, void *ctx);

/* Code banking: the banks instruction fetch and MOVC use (see above). */
void uam_mcs51_set_code_bank(uam_cpu *cpu, unsigned fetch_bank, unsigned movc_bank);

/* SFR paging: with automatic paging on, taking source n saves SFRPAGE on the core's
 * page stack and selects page[n]; RETI restores it. */
void uam_mcs51_set_sfr_autopage(uam_cpu *cpu, bool on);
uam_status uam_mcs51_set_irq_sfrpage(uam_cpu *cpu, unsigned source, uint8_t page);

/* Internal RAM, for loaders, tests and fuzz harnesses (side-effect free). */
uint8_t uam_mcs51_iram_read(uam_cpu *cpu, unsigned addr);
void    uam_mcs51_iram_write(uam_cpu *cpu, unsigned addr, uint8_t v);

/* True if `isa` is the 8051 descriptor. */
static inline bool uam_mcs51_is(const uam_isa *isa) { return isa == &uam_isa_mcs51; }

#endif /* UAM_ISA_MCS51_H */
