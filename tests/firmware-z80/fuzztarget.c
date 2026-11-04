/*
 * fuzztarget.c - A fuzz target with planted bugs for z80-ref (tests/fuzz/), built with
 * SDCC's z80 port and crt0.s. See tests/fuzz/fuzztarget.h for what it does. The UART is
 * the board's 16550 on I/O ports 0-7. A Z80 has no exceptions: run with --ram 0xFF00, the
 * bad store reaches nothing, which the harness sees as a store fault.
 */
#include <stdint.h>

#define __attribute__(x)          /* SDCC: no attributes, and nothing is inlined */

__sfr __at(0x00) UART_THR;
__sfr __at(0x05) UART_LSR;

#define BAD_ADDR 0xFF80u

#include "../fuzz/fuzztarget.h"
