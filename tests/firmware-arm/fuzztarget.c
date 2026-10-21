/*
 * fuzztarget.c - A fuzz target with planted bugs for arm-ref (tests/fuzz/), built as
 * Thumb-2 code. See tests/fuzz/fuzztarget.h for what it does.
 */
#include <stdint.h>

#define UART0    0x1c000000u
#define REG8(a)  (*(volatile uint8_t *)(uintptr_t)(a))
#define UART_THR REG8(UART0 + 0)
#define UART_LSR REG8(UART0 + 5)
#define BAD_ADDR 0x30000000u   /* nothing is mapped there: a data abort */

#include "../fuzz/fuzztarget.h"
