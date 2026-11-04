/* Not yet implemented: filled in by the following commits. */
#include "mips.h"
lift_result mips_lift_dsp(ml *L, uint32_t i) { (void)i; mips_ri(L); return LIFT_DONE; }
uint64_t mips_dsp_op(void *env, uint64_t insn, uint64_t b, uint64_t c, uam_status *st) { (void)env; (void)insn; (void)b; (void)c; (void)st; return 0; }
