/*
 * main.c - PC fixture firmware for tests/run-tests.sh: 32-bit protected mode, run in place
 * from flash, checking the PC example devices of ember-dev-build (references/examples/pc)
 * on the fixture board (fixture/pc/pcbrd_board.c). Prints to COM1, ends through the test
 * finisher at port 0x700. Devices the board was not built with are skipped.
 *
 * Boot 1: Super I/O (COM1 decode), PCI IDs, ECAM through PCIEXBAR, PAM shadowing, the ACPI
 * PM timer, an APM SMI with TSEG visible only in SMM, the SPI controller (descriptor,
 * read/write/erase cycles, the write gate, code rewritten in place), SMBus SPD reads; then
 * a reset through 0xCF9. Boot 2: "reset ok", the fuzz target, "done".
 */
#include <stdint.h>

#define RAM32(a) (*(volatile uint32_t *)(uintptr_t)(a))

static inline void outb(uint16_t p, uint8_t v)   { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p)); }
static inline void outw(uint16_t p, uint16_t v)  { __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(p)); }
static inline void outl(uint16_t p, uint32_t v)  { __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"(p)); }
static inline uint8_t inb(uint16_t p)  { uint8_t v;  __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint32_t inl(uint16_t p) { uint32_t v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }

#define COM1   0x3F8
#define PMBASE 0x600
#define SMBASE_IO 0xEFA0
#define SPIBAR 0xFE010000u
#define TSEG   0x03F00000u          /* 64 MiB of RAM, 1 MiB TSEG at its top */

/* ---- console and verdicts -------------------------------------------------------- */

static void putc_(char c) { while (!(inb(COM1 + 5) & 0x20)); outb(COM1, (uint8_t)c); }
static void puts_(const char *s) { for (; *s; s++) putc_(*s); }
static void hex(uint32_t v) { for (int i = 28; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]); }
static void finish(uint32_t v) { outl(0x700, v); for (;;) __asm__ volatile("hlt"); }
static void fail(unsigned n, const char *s, uint32_t got)
{
    puts_("FAIL "); puts_(s); puts_(" got "); hex(got); puts_("\n");
    finish((n << 16) | 0x3333);
}
#define CHECK(n, cond, what, got) do { if (!(cond)) fail(n, what, got); } while (0)

/* ---- PCI configuration through 0xCF8/0xCFC ---------------------------------------- */

static uint32_t cfg_addr(unsigned d, unsigned f, unsigned r)
{
    return 0x80000000u | d << 11 | f << 8 | (r & 0xFC);
}
static uint32_t pci_rd(unsigned d, unsigned f, unsigned r) { outl(0xCF8, cfg_addr(d, f, r)); return inl(0xCFC); }
static void pci_wr(unsigned d, unsigned f, unsigned r, uint32_t v) { outl(0xCF8, cfg_addr(d, f, r)); outl(0xCFC, v); }
static uint8_t pci_rd8(unsigned d, unsigned f, unsigned r) { outl(0xCF8, cfg_addr(d, f, r)); return inb((uint16_t)(0xCFC + (r & 3))); }
static void pci_wr8(unsigned d, unsigned f, unsigned r, uint8_t v) { outl(0xCF8, cfg_addr(d, f, r)); outb((uint16_t)(0xCFC + (r & 3)), v); }
static void pci_wr16(unsigned d, unsigned f, unsigned r, uint16_t v) { outl(0xCF8, cfg_addr(d, f, r)); outw((uint16_t)(0xCFC + (r & 2)), v); }

/* ---- Super I/O: COM1 decodes only once its logical device is programmed ----------- */

static int com1_present(void) { outb(COM1 + 7, 0x5A); return inb(COM1 + 7) == 0x5A; }

static void superio(void)
{
    outb(0x2E, 0x87); outb(0x2E, 0x87);                /* enter configuration mode */
    outb(0x2E, 0x20);
    uint8_t hi = inb(0x2F);
    outb(0x2E, 0x21);
    uint8_t lo = inb(0x2F);
    if (hi != 0x0C || lo != 0x11) {                    /* no Super I/O: COM1 is fixed */
        outb(0x2E, 0xAA);
        return;
    }
    if (com1_present())
        finish((1u << 16) | 0x3333);                   /* decoded before it was enabled */
    outb(0x2E, 0x07); outb(0x2F, 2);                   /* logical device 2: UART A */
    outb(0x2E, 0x60); outb(0x2F, 0x03);
    outb(0x2E, 0x61); outb(0x2F, 0xF8);
    outb(0x2E, 0x30); outb(0x2F, 0x01);                /* activate */
    outb(0x2E, 0xAA);                                  /* leave */
    if (!com1_present())
        finish((2u << 16) | 0x3333);
    puts_("sio ok\n");
}

/* ---- SPI hardware sequencing ------------------------------------------------------ */

static uint32_t spi_r(unsigned o) { return RAM32(SPIBAR + o); }
static void spi_w(unsigned o, uint32_t v) { RAM32(SPIBAR + o) = v; }

static int spi_cycle(unsigned cyc, uint32_t addr, unsigned n)
{
    spi_w(0x04, 0x7);                                  /* clear FDONE, FCERR, AEL */
    spi_w(0x08, addr);
    spi_w(0x04, (n - 1) << 24 | cyc << 17 | 1u << 16); /* FDBC, FCYCLE, FGO */
    uint32_t s;
    while (!((s = spi_r(0x04)) & 1))
        ;
    return !(s & 2);
}

typedef int (*xip_fn)(void);

static void spi(void)
{
    if ((pci_rd(31, 5, 0) & 0xFFFF) != 0x8086)
        return;
    pci_wr(31, 5, 0x10, SPIBAR);
    pci_wr16(31, 5, 0x04, 0x0006);                     /* memory decode, bus master */
    CHECK(30, spi_r(0x04) & (1u << 14), "spi FDV", spi_r(0x04));
    spi_w(0xB4, 0x0004);                               /* FDOC: map section, dword 1 (FLMAP0) */
    CHECK(31, spi_r(0xB8) == 0x00040003u, "spi FDOD", spi_r(0xB8));
    CHECK(32, spi_r(0x58) == 0x00FF0080u, "spi FREG1", spi_r(0x58));
    CHECK(33, spi_cycle(0, 0x10, 4) && spi_r(0x10) == 0x0FF0A55Au, "spi read", spi_r(0x10));
    spi_w(0x10, 0x12345678);
    CHECK(34, !spi_cycle(2, 0x80000, 4), "spi write without WPD", spi_r(0x04));
    pci_wr8(31, 5, 0xDC, pci_rd8(31, 5, 0xDC) | 1);    /* BIOS_CNTL.WPD */
    CHECK(35, spi_cycle(2, 0x80000, 4) && RAM32(0xFFF80000u) == 0x12345678u, "spi write",
          RAM32(0xFFF80000u));
    CHECK(36, spi_cycle(3, 0x80000, 1) && RAM32(0xFFF80000u) == 0xFFFFFFFFu, "spi erase",
          RAM32(0xFFF80000u));
    CHECK(37, !spi_cycle(2, 0x1000, 4), "spi ME region write", spi_r(0x04));
    /* code in the flash, run in place, rewritten through the controller */
    xip_fn f = (xip_fn)0xFFF81000u;
    CHECK(38, f() == 1, "xip before", (uint32_t)f());
    spi_w(0x10, 0x000002B8u);                          /* mov eax, 2 ; ret */
    spi_w(0x14, 0x0000C300u);
    CHECK(39, spi_cycle(3, 0x81000, 1) && spi_cycle(2, 0x81000, 6), "spi rewrite", spi_r(0x04));
    CHECK(40, f() == 2, "xip after", (uint32_t)f());
    puts_("spi ok\n");
}

/* ---- SMBus / SPD ------------------------------------------------------------------ */

static int smb_xfer(uint8_t slva, uint8_t cmd, unsigned proto, uint8_t *d0)
{
    outb(SMBASE_IO + 0, 0xFF);                         /* clear status */
    outb(SMBASE_IO + 4, slva);
    outb(SMBASE_IO + 3, cmd);
    outb(SMBASE_IO + 2, (uint8_t)(0x40 | proto << 2));
    uint8_t s;
    while (!((s = inb(SMBASE_IO + 0)) & 0x1E))
        ;
    if (d0)
        *d0 = inb(SMBASE_IO + 5);
    return (s & 0x1C) ? -1 : 0;
}

static void smbus(void)
{
    if ((pci_rd(31, 3, 0) & 0xFFFF) != 0x8086)
        return;
    pci_wr(31, 3, 0x20, SMBASE_IO | 1);
    pci_wr16(31, 3, 0x04, 0x0001);                     /* I/O decode */
    pci_wr8(31, 3, 0x40, 1);                           /* HOSTC.HST_EN */
    uint8_t v = 0;
    CHECK(50, smb_xfer(0x50 << 1 | 1, 2, 2, &v) == 0 && v == 0x0C, "spd type", v);
    CHECK(51, smb_xfer(0x37 << 1, 0, 0, 0) == 0, "spd page 1", 0);
    CHECK(52, smb_xfer(0x50 << 1 | 1, 0x40, 2, &v) == 0 && v == 0x80, "spd page 1 byte", v);
    smb_xfer(0x36 << 1, 0, 0, 0);
    CHECK(53, smb_xfer(0x51 << 1 | 1, 2, 2, &v) != 0, "empty slot", v);
    puts_("smbus ok\n");
}

/* ---- fuzz target (ember-fuzz check): input arrives on COM1 ------------------------ */

__attribute__((noinline)) void fuzz_bug(void) { for (;;) __asm__ volatile(""); }
__attribute__((noinline)) void fuzz_entry(void)
{
    char b[3] = { 0 };
    for (int i = 0; i < 3 && (inb(COM1 + 5) & 1); i++)
        b[i] = (char)inb(COM1);
    if (b[0] == 'B' && b[1] == 'U' && b[2] == 'G')
        fuzz_bug();
    if (b[0] == 'U' && b[1] == 'D' && b[2] == '!')
        __asm__ volatile("ud2");
}

/* ---- boot ------------------------------------------------------------------------- */

extern const char smi_handler[], smi_handler_end[];

void fw_main(uint32_t bist);
void fw_main(uint32_t bist)
{
    superio();
    if (RAM32(0x6000) == 0xB007B007u) {                /* the second boot, after the reset */
        RAM32(0x6000) = 0;
        puts_("reset ok\n");
        fuzz_entry();
        puts_("done\n");
        finish(0x5555);
    }
    puts_("pcfw boot bist="); hex(bist); puts_("\n");
    CHECK(10, pci_rd(0, 0, 0) == 0x29C08086u, "mch id", pci_rd(0, 0, 0));
    CHECK(11, pci_rd(31, 0, 0) == 0x29188086u, "lpc id", pci_rd(31, 0, 0));
    /* ECAM where PCIEXBAR puts it */
    pci_wr(0, 0, 0x64, 0);
    pci_wr(0, 0, 0x60, 0xE0000001u);                   /* 256 buses at 0xE0000000, enabled */
    CHECK(12, RAM32(0xE0000000u) == 0x29C08086u, "ecam mch", RAM32(0xE0000000u));
    CHECK(13, RAM32(0xE00F8000u) == 0x29188086u, "ecam lpc", RAM32(0xE00F8000u));
    /* PAM: 0xF0000 reads the flash until the segment's read enable is set */
    CHECK(14, RAM32(0xFFFF0u) == RAM32(0xFFFFFFF0u), "pam flash", RAM32(0xFFFF0u));
    pci_wr8(0, 0, 0x90, 0x30);                         /* PAM0: 0xF0000 read/write to RAM */
    RAM32(0xFFFF0u) = 0x5A5A1234u;
    CHECK(15, RAM32(0xFFFF0u) == 0x5A5A1234u, "pam ram", RAM32(0xFFFF0u));
    puts_("chipset ok\n");
    /* ACPI: the PM timer counts once PMBASE is decoded */
    pci_wr(31, 0, 0x40, PMBASE | 1);
    pci_wr8(31, 0, 0x44, 0x80);                        /* ACPI_CNTL.ACPI_EN */
    uint32_t t0 = inl(PMBASE + 8);
    for (volatile int i = 0; i < 2000; i++)
        ;
    CHECK(16, inl(PMBASE + 8) != t0, "pm timer", t0);
    /* SMM: TSEG hidden outside SMM, an APM SMI runs the handler at SMBASE + 0x8000 */
    RAM32(TSEG) = 0xCAFE0001u;
    pci_wr8(0, 0, 0x9D, 0x0A);                         /* SMRAM: G_SMRAME, C_BASE_SEG */
    pci_wr8(0, 0, 0x9E, 0x01);                         /* ESMRAMC: T_EN, 1 MiB TSEG */
    CHECK(17, RAM32(TSEG) == 0xFFFFFFFFu, "tseg hidden", RAM32(TSEG));
    for (const char *p = smi_handler; p < smi_handler_end; p++)
        *(volatile char *)(uintptr_t)(0x38000 + (p - smi_handler)) = *p;
    RAM32(0x5000) = 0;
    outl(PMBASE + 0x30, 0x21);                         /* SMI_EN: GBL_SMI_EN | APMC_EN */
    outb(0xB2, 0x5A);
    CHECK(18, RAM32(0x5000) == 0x534D4931u, "smi taken", RAM32(0x5000));
    CHECK(19, RAM32(0x5004) == 0xCAFE0001u, "tseg in smm", RAM32(0x5004));
    puts_("smm ok\n");
    spi();
    smbus();
    RAM32(0x6000) = 0xB007B007u;
    outb(0xCF9, 0x06);                                 /* reset control: system reset */
    fail(20, "cf9 reset", 0);
}
