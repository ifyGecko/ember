/*
 * fuzztarget.c - A fuzz target with planted bugs for mips-ref / mipsel-ref
 * (tests/fuzz/). See tests/fuzz/fuzztarget.h for what it does.
 */
#include <stdint.h>

#define UART0    0xbfd003f8u
#define REG8(a)  (*(volatile uint8_t *)(uintptr_t)(a))
#define UART_THR REG8(UART0 + 0)
#define UART_LSR REG8(UART0 + 5)
#define BAD_ADDR 0xbe000000u   /* kseg1 0x1e000000: nothing is mapped there */

#include "../fuzz/fuzztarget.h"
