/*
 * fuzztarget.c - A fuzz target with planted bugs for avr-ref (tests/fuzz/), built for the
 * default model (AVRe+, the atmega2560 layout). See tests/fuzz/fuzztarget.h for what it
 * does. An AVR has no exceptions: the bad store reaches nothing, which the harness sees
 * as a store fault.
 */
#include <stdint.h>

#define DEV_BASE 0x00E0u
#define REG8(a)  (*(volatile uint8_t *)(uintptr_t)(a))
#define UART_THR REG8(DEV_BASE + 0)
#define UART_LSR REG8(DEV_BASE + 5)
#define BAD_ADDR 0x4000u   /* past the end of SRAM: nothing in the data space */

#include "../fuzz/fuzztarget.h"
