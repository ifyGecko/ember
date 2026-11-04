/*
 * fuzztarget.c - A fuzz target with planted bugs for mcs51-ref (tests/fuzz/), built with
 * SDCC for the default model (8052). See tests/fuzz/fuzztarget.h for what it does. The
 * UART is the 8051's serial port: a byte is taken from SBUF and RI cleared for the
 * next. An 8051 has no exceptions: the bad store reaches nothing in external data, which
 * the harness sees as a store fault.
 */
#include <stdint.h>

#define __attribute__(x)          /* SDCC: no attributes, and nothing is inlined */

__sfr __at(0x98) SCON;
__sfr __at(0x99) SBUF;

static uint8_t uart_get(void)
{
    uint8_t b = SBUF;
    SCON &= (uint8_t)~0x01;       /* RI: the port loads the next byte */
    return b;
}

/* SDCC's start code calls this first: the receiver on (mode 1, REN). */
unsigned char __sdcc_external_startup(void)
{
    SCON = 0x50;
    return 0;
}

#define UART_TX_READY() 1         /* a byte written to SBUF is sent at once */
#define UART_RX_READY() (SCON & 0x01)
#define UART_PUT(c)     (SBUF = (uint8_t)(c))
#define UART_GET()      uart_get()
#define BAD_ADDR 0xFF80u          /* external data past the finisher: nothing there */

#include "../fuzz/fuzztarget.h"
