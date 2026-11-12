/*
 * cpu.c - Core services offered to ISA modules (uam/cpu.h).
 */
#include <stdlib.h>
#include <string.h>

#include "uam/bus.h"
#include "uam/cpu.h"
#include "uam/isa.h"

void uam_cpu_irq_sink(void *ctx, unsigned line, int level)
{
    uam_cpu *cpu = ctx;
    if (cpu->obs && cpu->obs->irq_input)
        cpu->obs->irq_input(cpu->obs->ctx, cpu, line, level);
    cpu->isa->irq_input(cpu, line, level);
    /* Interrupt state changed: re-evaluate at the next instruction boundary. */
    uam_cpu_request_exit(cpu);
}

uam_status uam_cpu_peek(uam_cpu *cpu, uint64_t addr, unsigned size, uint64_t *val)
{
    return uam_bus_read(cpu->bus, addr, size, UAM_ACC_LE | UAM_ACC_DEBUG, val);
}

/* Walk the <reg .../> elements of the target description: name="..." and the optional
 * regnum="..." (gdb numbers a register without one after the previous one). */
void uam_isa_reg_names(const uam_isa *isa, void (*fn)(void *ctx, const char *name, unsigned regno),
                       void *ctx)
{
    if (!isa->gdb || !isa->gdb->target_xml)
        return;
    unsigned next = 0;
    for (const char *p = strstr(isa->gdb->target_xml, "<reg "); p; p = strstr(p + 1, "<reg ")) {
        const char *end = strchr(p, '>');
        if (!end)
            break;
        char name[48] = "";
        const char *n = strstr(p, " name=\"");
        if (n && n < end) {
            n += 7;
            size_t len = strcspn(n, "\"");
            if (len < sizeof(name)) {
                memcpy(name, n, len);
                name[len] = 0;
            }
        }
        const char *r = strstr(p, " regnum=\"");
        unsigned num = r && r < end ? (unsigned)strtoul(r + 9, NULL, 0) : next;
        next = num + 1;
        if (name[0])
            fn(ctx, name, num);
    }
}

typedef struct reg_find {
    const char *name;
    unsigned    regno;
    bool        found;
} reg_find;

static void reg_match(void *ctx, const char *name, unsigned regno)
{
    reg_find *f = ctx;
    if (!f->found && !strcmp(name, f->name)) {
        f->regno = regno;
        f->found = true;
    }
}

bool uam_isa_reg_by_name(const uam_isa *isa, const char *name, unsigned *regno)
{
    reg_find f = { .name = name };
    uam_isa_reg_names(isa, reg_match, &f);
    if (f.found)
        *regno = f.regno;
    return f.found;
}
