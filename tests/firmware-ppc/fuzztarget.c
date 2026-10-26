/*
 * fuzztarget.c - A fuzz target with planted bugs for ppc-ref (tests/fuzz/). See
 * tests/fuzz/fuzztarget.h for what it does.
 */
#include <stdint.h>

#define UART0    0xf0000000u
#define REG8(a)  (*(volatile uint8_t *)(uintptr_t)(a))
#define UART_THR REG8(UART0 + 0)
#define UART_LSR REG8(UART0 + 5)
#define BAD_ADDR 0xe0000000u   /* nothing is mapped there: a machine check */

#include "../fuzz/fuzztarget.h"
