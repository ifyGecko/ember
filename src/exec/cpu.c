/*
 * cpu.c - Core services offered to ISA modules (uam/cpu.h).
 */
#include "uam/bus.h"
#include "uam/cpu.h"
#include "uam/isa.h"

void uam_cpu_irq_sink(void *ctx, unsigned line, int level)
{
    uam_cpu *cpu = ctx;
    cpu->isa->irq_input(cpu, line, level);
    /* Interrupt state changed: re-evaluate at the next instruction boundary. */
    uam_cpu_request_exit(cpu);
}

uam_status uam_cpu_peek(uam_cpu *cpu, uint64_t addr, unsigned size, uint64_t *val)
{
    return uam_bus_read(cpu->bus, addr, size, UAM_ACC_LE | UAM_ACC_DEBUG, val);
}
