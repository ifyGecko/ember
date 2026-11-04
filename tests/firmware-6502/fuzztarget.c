/*
 * fuzztarget.c - A fuzz target with planted bugs for 6502-ref (tests/fuzz/), built with
 * SDCC (mos6502) for the NMOS model. See tests/fuzz/fuzztarget.h for what it does. The
 * UART is the board's 16550. A 6502 has no exceptions: the bad store reaches an
 * address in the I/O page that nothing answers, which the harness sees as a store fault.
 */
#include <stdint.h>

#define __attribute__(x)          /* SDCC: no attributes, and nothing is inlined */

#define UART_THR (*(volatile uint8_t *)0x7F00)
#define UART_LSR (*(volatile uint8_t *)0x7F05)
#define BAD_ADDR 0x7F80u          /* in the I/O page, past the devices: nothing there */

/* SDCC's start code calls this first; 0: initialise the data as usual. */
unsigned char __sdcc_external_startup(void)
{
    return 0;
}

#include "../fuzz/fuzztarget.h"
