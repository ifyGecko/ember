/*
 * uam/isa/x86.h - Public face of the x86-64 ISA module.
 *
 * Boards use this to instantiate an x86-64 hart. The module resets into
 * 16-bit real mode at the architectural reset vector (physical 0xFFFFFFF0,
 * CS base 0xFFFF0000) and brings itself up through protected and long mode
 * as the firmware loads CR0/CR4/EFER and paging - the classic PC firmware
 * path. See docs/uam-isa.md for the IR and docs/architecture.md (section 5,
 * "Legacy PC") for the module.
 *
 * x86 I/O ports are not a separate address space in UAM: the lifter turns
 * IN/OUT into ordinary loads and stores at a reserved physical aperture
 * (UAM_X86_PIO_BASE + port), so port devices are ordinary MMIO devices and
 * the whole bus/snapshot/mmio-rule/coverage machinery applies unchanged.
 */
#ifndef UAM_ISA_X86_H
#define UAM_ISA_X86_H

#include "uam/isa.h"

/* x86-64 (IA-32e capable), starting in real mode at the reset vector. */
extern const uam_isa uam_isa_x86_64;

/*
 * The physical aperture the 64 KiB I/O-port space is mapped into. A board
 * maps its port devices (PIT, PIC, RTC, PCI CAM, the PM block, ...) at
 * UAM_X86_PIO_BASE + port. It sits above the 4 GiB firmware/flash window and
 * well clear of the 32-bit RAM and MMIO a PC uses. Paging never applies to
 * it: IN/OUT are physical accesses (UAM_ACC_PHYS).
 */
#define UAM_X86_PIO_BASE   0x0000010000000000ull  /* 2^40 */
#define UAM_X86_PIO_SIZE   0x10000ull             /* ports 0x0000..0xFFFF */

static inline uint64_t uam_x86_pio(uint16_t port) { return UAM_X86_PIO_BASE + port; }

/*
 * Outside SMM, accesses to the SMRAM the processor itself protects - the SMM
 * range registers' range (IA32_SMRR_*) and, with the AMD64 map, the TSeg
 * (SMM_ADDR/SMM_MASK) - are redirected to UAM_X86_SMRR_SINK + (address -
 * range base); so are SMM's data accesses to a TSeg closed by SMM_MASK.TClose.
 * Boards leave the sink unmapped, so the access gets the board's
 * unclaimed-access behaviour (a PC master-aborts: reads all ones, writes
 * dropped), or map a device there to observe it.
 */
#define UAM_X86_SMRR_SINK  0x0000800000000000ull  /* 2^47 */

/*
 * The SMM view. A chipset decodes SMRAM by SMIACT#: the same address is SMRAM
 * to a processor in SMM and something else (VGA, a closed hole, plain DRAM) to
 * everyone else. Here a processor in SMM presents its accesses to an SMRAM
 * window on a separate aperture, UAM_X86_SMM_VIEW + address, where the board
 * maps SMRAM (RAM, or an alias of the DRAM behind it); the address itself
 * keeps the normal decode, which other processors, DMA and the debugger see.
 * Every processor decides by its own state, so nothing is switched on SMM
 * entry and exit.
 *
 * The windows: the board's (uam_x86_set_smram_window: Intel's compatible
 * SMRAM at A0000 and TSEG, as the chipset's SMRAM registers open them) and,
 * with the AMD64 map, the ASeg (A0000-BFFFF) while SMM_MASK.AValid is set.
 * Instruction fetches always use the view; data accesses use the normal
 * decode where the window is closed for data (UAM_X86_SMRAM_DATA_CLOSED,
 * Intel's D_CLS; AMD's AClose). Page walks and the save state are data.
 * To read SMRAM as SMM sees it from outside (gdb, --dump-mem p:), add
 * UAM_X86_SMM_VIEW to its address.
 */
#define UAM_X86_SMM_VIEW   0x0000400000000000ull  /* 2^46 */
#define UAM_X86_SMRAM_WINDOWS 8u

enum {
    /* SMM data accesses to the window use the normal decode; fetches stay SMRAM */
    UAM_X86_SMRAM_DATA_CLOSED = 1u << 0,
};

/* Set window `slot` (< UAM_X86_SMRAM_WINDOWS) to [base, base + size); size 0
 * removes it. A chipset model calls it for every processor whenever its SMRAM
 * registers change (and at its reset). Kept across processor RESET and saved
 * in snapshots. */
void uam_x86_set_smram_window(uam_cpu *cpu, unsigned slot, uint64_t base, uint64_t size,
                              unsigned flags);

/*
 * Interrupt input lines, as numbered for isa->irq_input. A PC has one wire
 * into the core (INTR, from the local APIC or the legacy 8259), plus the
 * non-maskable and system-management pins, and INIT (a soft reset that keeps
 * memory and caches; used by multiprocessor bring-up). The board's interrupt
 * controller drives UAM_X86_IRQ_INTR; a chipset SMI source drives
 * UAM_X86_IRQ_SMI.
 */
enum uam_x86_irq_line {
    UAM_X86_IRQ_INTR = 0,   /* maskable interrupt pin (RFLAGS.IF gates it) */
    UAM_X86_IRQ_NMI  = 1,   /* non-maskable interrupt (edge: taken once per rise) */
    UAM_X86_IRQ_SMI  = 2,   /* system-management interrupt -> SMM entry (edge) */
    UAM_X86_IRQ_INIT = 3,   /* INIT# (edge) */
    UAM_X86_IRQ_A20M = 4,   /* A20M# (level): high forces physical address bit 20 to 0
                             * (the PC's A20 gate; ignored in SMM) */
};

/*
 * Platform hooks: the few places the core reaches out to the chipset. All are
 * optional (NULL: the default noted). A board sets them once after creating
 * the hart, typically pointing them at its interrupt controller and chipset
 * models. They run synchronously on the hart's thread.
 */
typedef struct uam_x86_platform {
    void *ctx;
    /* INTR acknowledge (the INTA cycle): the vector of the highest-priority
     * pending interrupt, marked in service by the controller. Return -1 if
     * nothing is pending any more (a spurious INTR). Default: -1. */
    int  (*inta)(void *ctx);
    /* RDMSR/WRMSR the core does not implement itself (x2APIC, chipset and
     * vendor MSRs). Return true if handled. Unhandled MSRs fall back to the
     * module's MSR policy (uam_x86_set_msr_policy). */
    bool (*rdmsr)(void *ctx, uint32_t msr, uint64_t *val);
    bool (*wrmsr)(void *ctx, uint32_t msr, uint64_t val);
    /* The hart entered (true) or left (false) SMM - SMIACT#; also called when
     * a processor is switched in (several processors). SMRAM decode no longer
     * needs it - map SMRAM in the SMM view (UAM_X86_SMM_VIEW) - but a chipset
     * may still track SMIACT# for its own registers. */
    void (*smm_changed)(void *ctx, bool in_smm);
    /* CPUID: called after the module has computed its answer r[0..3] (EAX,
     * EBX, ECX, EDX) for (leaf, subleaf); the board may change any of it. For
     * what the module does not describe itself: vendor-specific leaves (an
     * extended signature in 0x80000001, topology, cache descriptors) or a
     * raised maximum leaf. Default: the module's answer. */
    void (*cpuid)(void *ctx, uint32_t leaf, uint32_t subleaf, uint32_t r[4]);
} uam_x86_platform;

void uam_x86_set_platform(uam_cpu *cpu, const uam_x86_platform *p);

/*
 * What RDMSR/WRMSR of an MSR nobody implements does. Firmware probes many
 * model-specific registers; during bring-up it is usually more useful to
 * remember writes and read back what was written (or 0) than to stop.
 */
typedef enum uam_x86_msr_policy {
    UAM_X86_MSR_STORE = 0,  /* remember writes; reads return the last write or 0 (default) */
    UAM_X86_MSR_GP,         /* raise #GP(0), as hardware does for a missing MSR */
} uam_x86_msr_policy;

void uam_x86_set_msr_policy(uam_cpu *cpu, uam_x86_msr_policy p, bool trace);

/*
 * The time-stamp counter counts virtual-time ticks (TSC frequency = the
 * machine's tick rate), idle time included.
 *
 * The processor identity CPUID reports. The defaults describe a generic
 * x86-64 processor with the features this module implements (no AVX, no
 * VMX) and name no vendor ("UAMx86RefCPU", family 6 model 0); a board for a
 * specific platform sets the vendor, the family/model/stepping signature,
 * the brand string and the logical processor count, which firmware uses to
 * pick microcode and silicon paths.
 */
typedef struct uam_x86_cpuid_info {
    char     vendor[13];      /* 12 characters, leaf 0 EBX/EDX/ECX */
    uint32_t signature;       /* leaf 1 EAX: stepping/model/family */
    char     brand[49];       /* leaves 0x80000002..4 */
    unsigned logical_cpus;    /* leaf 1 EBX[23:16], leaf 4/0xB topology */
    unsigned phys_addr_bits;  /* leaf 0x80000008 EAX[7:0] */
    unsigned initial_apic_id; /* leaf 1 EBX[31:24] */
    unsigned features;        /* UAM_X86_FEAT_*: what the board's devices add */
} uam_x86_cpuid_info;

/* Features that depend on the platform's devices, reported only when the
 * board sets them: the local APIC's x2APIC mode (also lets software set
 * IA32_APIC_BASE.EXTD) and its TSC-deadline timer. */
enum {
    UAM_X86_FEAT_X2APIC       = 1u << 0,
    UAM_X86_FEAT_TSC_DEADLINE = 1u << 1,
    /* Intel's SMM code-access check: MSR_SMM_MCA_CAP (17Dh) reports it and
     * MSR_SMM_FEATURE_CONTROL (4E0h: lock, SMM_Code_Chk_En) enables it; both
     * only in SMM. Enabled, an SMM instruction fetch outside the SMRR range is
     * an unrecoverable machine check: #MC with CR4.MCE set, else shutdown. */
    UAM_X86_FEAT_SMM_CODE_CHK = 1u << 2,
    /* Intel's MSR_SMM_BLOCKED (4E3h: the package's processors that hold an SMI,
     * i.e. wait for a startup IPI) and MSR_SMM_DELAYED (4E2h: always 0 here),
     * with MSR_SMM_MCA_CAP.LONG_FLOW_INDICATION; SMM only. SMM rendezvous code
     * uses them not to wait for processors that cannot arrive. The bits follow
     * uam_x86_set_package. */
    UAM_X86_FEAT_SMM_BLOCKED  = 1u << 3,
};

void uam_x86_set_cpuid(uam_cpu *cpu, const uam_x86_cpuid_info *info);

/*
 * The SMRAM save-state map an SMI writes below SMBASE + 0x10000 and RSM reads
 * back: the processor vendor's, which its SMM handler (SMBASE relocation, I/O
 * trap decoding) is written for. The two overlap, so the handler and the map
 * must agree. Set per processor, like the identity; kept across reset and INIT.
 *   INTEL64  Intel 64 (default): SMBASE + 0xFC00..0xFFFF; SMBASE at +0xFEF8,
 *            RIP at +0xFFD8, I/O restart at +0xFF00, IOMisc at +0xFFA4.
 *   AMD64    AMD64: SMBASE + 0xFE00..0xFFFF; SMBASE at +0xFF00, RIP at
 *            +0xFF78, I/O restart at +0xFEC8, SMM I/O trap at +0xFEC0; also
 *            the SMM_BASE MSR (C001_0111h), which reads and sets SMBASE, and
 *            AMD's SMRAM protection, which the core then keeps: SMM_ADDR
 *            (C001_0112h) and SMM_MASK (C001_0113h) - the TSeg, closed to
 *            accesses outside SMM, and the AValid/TValid/AClose/TClose bits -
 *            and HWCR (C001_0015h), whose SMMLOCK makes SMM_BASE, SMM_ADDR and
 *            SMM_MASK read-only (writes ignored) until RESET. The ASeg
 *            (A0000-BFFFF) is an SMM-view window while AValid is set (closed
 *            for data by AClose): the board maps the DRAM behind it at
 *            UAM_X86_SMM_VIEW + 0xA0000. The TSeg needs no view mapping: SMM
 *            reaches its DRAM at its own address, everyone else the sink.
 */
typedef enum uam_x86_smm_map {
    UAM_X86_SMM_MAP_INTEL64 = 0,
    UAM_X86_SMM_MAP_AMD64,
} uam_x86_smm_map;

void uam_x86_set_smm_map(uam_cpu *cpu, uam_x86_smm_map map);

/*
 * AMD64 map: the SMM configuration that is in place before the x86 cores run
 * (on AMD platforms the security processor programs it): SMM_ADDR, SMM_MASK
 * and HWCR.SMMLOCK as they read at every RESET from now on. Applied at once.
 */
void uam_x86_preset_amd_smm(uam_cpu *cpu, uint64_t smm_addr, uint64_t smm_mask, bool lock);

/*
 * SMM code-fetch confinement, a check hardware does not make on every
 * processor: while in SMM, an instruction fetch from outside SMRAM (the SMRR
 * range, and with the AMD64 map the valid TSeg and ASeg) is handled like a
 * failed Intel SMM code-access check - a machine check - and reported as
 * UAM_EXC_FETCH_FAULT at the fetching pc (an exception hook, e.g. the fuzz
 * harness, sees it first). Inactive until one of those ranges is defined.
 * Kept across RESET. For finding SMM call-outs (a handler calling or jumping
 * into memory the OS controls).
 */
void uam_x86_set_smm_confine(uam_cpu *cpu, bool on);

/*
 * The logical processors of one physical package, in the bit order of the
 * package-scope MSR_SMM_BLOCKED/MSR_SMM_DELAYED (UAM_X86_FEAT_SMM_BLOCKED).
 * Call once per package after creating its processors; kept across RESET.
 */
uam_status uam_x86_set_package(uam_cpu *const *cpus, unsigned n);

/*
 * Multiprocessor start-up. Processor 0 (hartid 0) is the bootstrap processor
 * (IA32_APIC_BASE.BSP); every other processor leaves reset, and INIT, in the
 * wait-for-SIPI state, where only INIT and a startup IPI are recognised; an
 * SMI stays pending until the startup IPI, as on hardware (MSR_SMM_BLOCKED
 * reports such processors), and is taken before the first instruction. A
 * startup IPI with `vector` starts it in real mode at vector << 12 (CS =
 * vector << 8). The local APIC model delivers it through this call.
 */
void uam_x86_sipi(void *cpu, uint8_t vector);   /* `cpu` is the uam_cpu */
/* The identity in effect (the defaults until a board sets one). */
void uam_x86_get_cpuid(uam_cpu *cpu, uam_x86_cpuid_info *info);

/* The processor's current TSC (IA32_TSC), for devices that compare against
 * it (a local APIC's TSC-deadline timer: uam_lapic_set_tsc). `cpu` is the
 * uam_cpu. */
uint64_t uam_x86_read_tsc(void *cpu);

#endif /* UAM_ISA_X86_H */
