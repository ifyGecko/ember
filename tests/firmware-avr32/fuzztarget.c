/*
 * fuzztarget.c - A fuzz target with planted bugs for avr32-ref (AVR32A, tests/fuzz/). See
 * tests/fuzz/fuzztarget.h for what it does.
 */
#include <stdint.h>

#define UART0    0xffff1400u
#define REG8(a)  (*(volatile uint8_t *)(uintptr_t)(a))
#define UART_THR REG8(UART0 + 0)
#define UART_LSR REG8(UART0 + 5)
#define BAD_ADDR 0x40000000u   /* nothing is mapped there */

#include "../fuzz/fuzztarget.h"

/* start.S's interrupt stubs call this; the target enables no interrupts */
void irq_handler(unsigned level);
void irq_handler(unsigned level) { (void)level; }
