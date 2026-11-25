/*
 * debug.c - A debugger's access to guest memory and registers (uam/debug.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/debug.h"

bool uam_debug_vtop(uam_cpu *cpu, uint64_t va, uint64_t *pa)
{
    if (!cpu->isa->translate) {
        *pa = va;
        return true;
    }
    uam_xlate xl = { 0 };
    if (cpu->isa->translate(cpu, va, UAM_XLATE_DEBUG, &xl) != UAM_OK)
        return false;
    *pa = xl.paddr;
    return true;
}

static void say(char *why, size_t why_len, const char *fmt, unsigned long long a, const char *s1,
                const char *s2)
{
    if (why && why_len)
        snprintf(why, why_len, fmt, a, s1, s2);
}

/* Why the bus refused a debug access at pa. */
static void refused(const uam_machine *m, uint64_t pa, bool write, char *why, size_t why_len)
{
    const uam_region *r = uam_bus_find(&m->bus, pa);
    if (!r)
        say(why, why_len, "nothing is mapped at physical 0x%llx%s%s", (unsigned long long)pa, "", "");
    else if (!write && r->kind == UAM_REGION_MMIO && r->dev && !r->dev->ops->peek)
        say(why, why_len, "physical 0x%llx is device region '%s', whose device (%s) has no "
            "side-effect-free read (uam_device_ops.peek)", (unsigned long long)pa, r->name,
            r->dev->ops->type);
    else
        say(why, why_len, "physical 0x%llx: region '%s' refused a debug %s", (unsigned long long)pa,
            r->name ? r->name : "?", write ? "write" : "read");
}

uint64_t uam_debug_read(uam_machine *m, uam_cpu *cpu, uint64_t addr, bool phys, void *buf,
                        uint64_t len, char *why, size_t why_len)
{
    uint8_t *out = buf;
    for (uint64_t i = 0; i < len; i++) {
        uint64_t pa = addr + i, b;
        if (!phys && !uam_debug_vtop(cpu, addr + i, &pa)) {
            say(why, why_len, "virtual 0x%llx is not mapped for this processor%s%s",
                (unsigned long long)(addr + i), "", "");
            return i;
        }
        if (uam_bus_read(&m->bus, pa, 1, UAM_ACC_LE | UAM_ACC_DEBUG, &b) != UAM_OK) {
            refused(m, pa, false, why, why_len);
            return i;
        }
        out[i] = (uint8_t)b;
    }
    return len;
}

uint64_t uam_debug_write(uam_machine *m, uam_cpu *cpu, uint64_t addr, bool phys,
                         const void *buf, uint64_t len, char *why, size_t why_len)
{
    const uint8_t *in = buf;
    for (uint64_t i = 0; i < len; i++) {
        uint64_t pa = addr + i;
        if (!phys && !uam_debug_vtop(cpu, addr + i, &pa)) {
            say(why, why_len, "virtual 0x%llx is not mapped for this processor%s%s",
                (unsigned long long)(addr + i), "", "");
            return i;
        }
        if (uam_bus_write(&m->bus, pa, 1, UAM_ACC_LE | UAM_ACC_DEBUG, in[i]) != UAM_OK) {
            refused(m, pa, true, why, why_len);
            return i;
        }
    }
    return len;
}

bool uam_debug_parse_addr(const char *text, uint64_t *addr, bool *phys)
{
    *phys = !strncmp(text, "p:", 2);
    const char *s = *phys ? text + 2 : text;
    char *end;
    *addr = strtoull(s, &end, 0);
    return end != s && !*end;
}

bool uam_debug_parse_range(const char *text, uam_debug_range *r)
{
    char *end;
    *r = (uam_debug_range){ 0 };
    const char *s = text;
    if (!strncmp(s, "p:", 2)) {
        r->phys = true;
        s += 2;
    }
    r->addr = strtoull(s, &end, 0);
    if (end == s || *end != '+')
        return false;
    s = end + 1;
    r->len = strtoull(s, &end, 0);
    if (end == s)
        return false;
    switch (*end) {
    case 'k': case 'K': r->len <<= 10; end++; break;
    case 'm': case 'M': r->len <<= 20; end++; break;
    case 'g': case 'G': r->len <<= 30; end++; break;
    default: break;
    }
    if (*end == '=' && end[1])
        r->file = end + 1;
    else if (*end)
        return false;
    return r->len > 0 && r->len <= (UINT64_C(1) << 30);
}

unsigned uam_debug_reg_count(const uam_cpu *cpu)
{
    return cpu->isa->gdb ? cpu->isa->gdb->n_regs : 0;
}

unsigned uam_debug_reg_size(const uam_cpu *cpu, unsigned regno)
{
    const uam_gdb_arch *a = cpu->isa->gdb;
    if (!a || regno >= a->n_regs)
        return 0;
    return a->reg_sizes ? a->reg_sizes[regno] : a->reg_bytes;
}

bool uam_debug_reg_read(uam_cpu *cpu, unsigned regno, uint64_t *val, uint8_t *buf)
{
    const uam_gdb_arch *a = cpu->isa->gdb;
    unsigned n = uam_debug_reg_size(cpu, regno);
    if (!n)
        return false;
    if (n > 8)
        return n <= UAM_DEBUG_REG_MAX && a->read_wide && buf && a->read_wide(cpu, regno, buf);
    uint64_t v = 0;
    if (!a->read_reg || !a->read_reg(cpu, regno, &v))
        return false;
    if (val)
        *val = v;
    return true;
}

bool uam_debug_reg_write(uam_cpu *cpu, unsigned regno, uint64_t val, const uint8_t *buf)
{
    const uam_gdb_arch *a = cpu->isa->gdb;
    unsigned n = uam_debug_reg_size(cpu, regno);
    if (!n)
        return false;
    if (n > 8)
        return n <= UAM_DEBUG_REG_MAX && a->write_wide && buf && a->write_wide(cpu, regno, buf);
    return a->write_reg && a->write_reg(cpu, regno, val);
}
