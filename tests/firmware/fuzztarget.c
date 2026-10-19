/*
 * fuzztarget.c - A fuzz target with planted bugs for rv32-ref (tests/fuzz/).
 *
 * After booting, main calls fuzz_iteration() forever: it drains the UART receive FIFO
 * into a buffer and parses it. A harness begins every iteration at fuzz_iteration
 * (begin-only mode: the next call ends it) and delivers inputs through "uart0".
 *   "FUZ!"  a store to an unmapped address (bus fault)
 *   "FUZ?"  fuzz_panic(), the firmware's fatal-error routine (a crash pc)
 *   "L..."  a loop that never ends (timeout)
 * Any exception the harness lets through ends in fuzz_panic as well.
 */
#include <stdint.h>

#define UART0    0x10000000u
#define REG8(a)  (*(volatile uint8_t *)(uintptr_t)(a))
#define UART_THR REG8(UART0 + 0)
#define UART_LSR REG8(UART0 + 5)
#define BAD_ADDR 0x40000000u   /* nothing is mapped there */

#include "../fuzz/fuzztarget.h"
