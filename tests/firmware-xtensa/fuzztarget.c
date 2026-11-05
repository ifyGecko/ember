/*
 * fuzztarget.c - A fuzz target with planted bugs for xtensa-ref (tests/fuzz/). See
 * tests/fuzz/fuzztarget.h for what it does; the start code turns every exception into
 * trap_handler.
 */
#include <stdint.h>

#define UART0    0x60000000u
#define REG8(a)  (*(volatile uint8_t *)(uintptr_t)(a))
#define UART_THR REG8(UART0 + 0)
#define UART_LSR REG8(UART0 + 5)
#define BAD_ADDR 0x30000000u   /* nothing is mapped there: LoadStoreError */

#include "../fuzz/fuzztarget.h"
