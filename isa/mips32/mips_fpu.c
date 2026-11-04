/* Not yet implemented: filled in by the following commits. */
#include "mips.h"
lift_result mips_lift_fpu(ml *L, uint32_t i) { (void)i; mips_trap(L, NONE, EXC_CPU, mips_imm(L, 1)); return LIFT_DONE; }
uint64_t mips_fpu_op(void *env, uint64_t insn, uint64_t b, uint64_t c, uam_status *st) { (void)env; (void)insn; (void)b; (void)c; (void)st; return 0; }
uint64_t mips_cfc1(void *env, uint64_t fs, uint64_t b, uint64_t c, uam_status *st) { (void)env; (void)fs; (void)b; (void)c; (void)st; return 0; }
uint64_t mips_ctc1(void *env, uint64_t fs, uint64_t v, uint64_t c, uam_status *st) { (void)env; (void)fs; (void)v; (void)c; (void)st; return 0; }
