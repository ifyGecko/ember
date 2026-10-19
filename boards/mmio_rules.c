/*
 * mmio_rules.c - Per-register stubs for any board (uam_board_add_mmio_rules, uam/boards.h).
 */
#include <stdio.h>

#include "uam/boards.h"

uam_status uam_board_add_mmio_rules(uam_machine *m, const uam_mmio_rule *rules, size_t n,
                                    uam_mmio_input *in)
{
    if (n > UAM_MAX_MMIO_RULES) {
        uam_log(UAM_LOG_ERROR, "mmio-rules", "%zu rules given, at most %u are supported", n,
                UAM_MAX_MMIO_RULES);
        return UAM_ERR_INVALID;
    }
    for (size_t i = 0; i < n; i++) {
        char name[48];
        snprintf(name, sizeof(name), "rule@0x%llx", (unsigned long long)rules[i].addr);
        uam_device *d = uam_mmio_stub_create(name, &rules[i], in);
        if (!d)
            return UAM_ERR_INVALID;
        uam_machine_add_device(m, d);
        const uam_map_attrs attrs = { .prio = UAM_PRIO_STUB + (int)i,
                                      .endian = m->cpu.isa->default_endian };
        uam_status st = uam_bus_map_mmio_ex(&m->bus, d->name, rules[i].addr, rules[i].size, d,
                                            &attrs);
        if (st != UAM_OK)
            return st;
    }
    if (in)
        return uam_machine_add_state(m, "mmio-input", uam_mmio_input_save,
                                     uam_mmio_input_restore, in);
    return UAM_OK;
}
