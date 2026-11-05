/*
 * testbrd_hle.c - firmware functions replaced by native code (PC hooks). Each entry is a
 * place where the emulation does NOT behave like the hardware: keep the list short, cite
 * the function and the iteration that justified it.
 *
 *   0x800001c0 puts_  -> returns at once (iter-07: example only)
 */
#include <string.h>

#include "testbrd_hle.h"

static const uam_regdesc *reg(uam_cpu *cpu, const char *name)
{
    for (size_t i = 0; i < cpu->isa->n_regs; i++)
        if (strcmp(cpu->isa->regs[i].name, name) == 0)
            return &cpu->isa->regs[i];
    return NULL;
}

/* Emulate "return v": result register, then jump to the return address. Hook a
 * function's first instruction (never a delay slot). */
static void hle_return(uam_cpu *cpu, uint64_t v)
{
    const uam_regdesc *ret = reg(cpu, "v0"), *ra = reg(cpu, "ra");   /* RISC-V: "a0", "ra" */
    uam_cpu_reg_write(cpu, ret->offset, ret->width, v);
    cpu->pc = uam_cpu_reg_read(cpu, ra->offset, ra->width);
}

static uam_hook_action skip_puts(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    (void)ctx;
    (void)pc;
    hle_return(cpu, 0);
    return UAM_HOOK_CONTINUE;
}

uam_status testbrd_hle_install(uam_machine *m, const uam_board_config *cfg)
{
    if (!uam_board_config_option(cfg, "hle"))       /* opt-in while it is being evaluated */
        return UAM_OK;
    return uam_exec_add_pc_hook(&m->exec, 0x800001c0, skip_puts, NULL);
}
