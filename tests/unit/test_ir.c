/*
 * test_ir.c - Unit tests for UAM-IR: semantics (docs/uam-isa.md section 7),
 * guards, memory ops, precise faults, and the verifier (section 8).
 *
 * Each test builds a tiny block, verifies it, and runs it on a bare hart with
 * 64 KiB of RAM at 0x1000 and no ISA (no SYSRD/SYSWR/HELPER are used).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/bus.h"
#include "uam/exec.h"
#include "uam/ir.h"

#define RAM_BASE 0x1000u
#define REGFILE  64u
#define NONE     UAM_REF_NONE

static int failures, checks;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        checks++;                                                     \
        if (!(cond)) {                                                \
            failures++;                                               \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);      \
            fprintf(stderr, __VA_ARGS__);                             \
            fputc('\n', stderr);                                      \
        }                                                             \
    } while (0)

/* ---- harness ----------------------------------------------------------- */

static uam_bus     bus;
static uam_cpu     cpu;
static uint8_t     regfile[REGFILE];
static uam_builder B;
static uint64_t    temps[256];
static const uam_isa test_isa = { .name = "test", .addr_width = 32, .regfile_size = REGFILE };

static void setup(void)
{
    uam_bus_init(&bus);
    if (uam_bus_map_ram(&bus, "ram", RAM_BASE, 0x10000) != UAM_OK) {
        fprintf(stderr, "cannot map RAM\n");
        exit(2);
    }
    memset(&cpu, 0, sizeof(cpu));
    cpu.regfile = regfile;
    cpu.regfile_size = REGFILE;
    cpu.bus = &bus;
    cpu.isa = &test_isa;
    uam_builder_init(&B, 32);
}

static void begin(void)
{
    memset(regfile, 0, sizeof(regfile));
    cpu.exit_request = false;
    cpu.icount = 0;
    cpu.icount_deadline = UINT64_MAX;
    cpu.resv.valid = false;
    uam_builder_begin(&B, 0x100);
    uam_imark(&B, 0x100, 4);
}

static uam_block *finish_checked(void)
{
    uam_block *blk = uam_builder_finish(&B);
    char err[256];
    uam_verify_ctx vc = { .regfile_size = REGFILE };
    if (!uam_verify(blk, &vc, err, sizeof(err))) {
        fprintf(stderr, "unexpected verifier failure: %s\n", err);
        uam_print_block(stderr, blk, NULL);
        exit(2);
    }
    return blk;
}

static uam_block_result run(uam_block *blk)
{
    uam_block_result r = uam_interp_block(&cpu, blk, NULL, temps);
    uam_block_free(blk);
    return r;
}

/* Start another guest instruction (the commit rule is per instruction). */
static void next_insn(void)
{
    static uint64_t pc = 0x104;
    uam_imark(&B, pc, 4);
    pc += 4;
}

static uint64_t reg64(uint32_t off) { return uam_cpu_reg_read(&cpu, off, 64); }

/* Store a result of any width into reg slot 0 as 64 bits, then exit. */
static void put_result(uam_ref v, unsigned w)
{
    if (w < 64)
        v = uam_unop(&B, UAM_OP_ZEXT, 64, v);
    uam_put(&B, 64, 0, v, NONE);
    uam_exit(&B, NONE, uam_imm(&B, 0x104), UAM_EXIT_JUMP);
}

/* Evaluate a binary op on immediates. */
static uint64_t bin(uam_opcode op, unsigned w, uint64_t x, uint64_t y)
{
    begin();
    uam_ref r = uam_binop(&B, op, w, uam_imm(&B, x & uam_mask(w)), uam_imm(&B, y & uam_mask(w)));
    unsigned rw = uam_opinfo_table[op].result == UAM_RK_BOOL ? 1 : w;
    put_result(r, rw);
    run(finish_checked());
    return reg64(0);
}

/* Evaluate a unary op on a register-sourced temp (conversions need a temp). */
static uint64_t un(uam_opcode op, unsigned w, unsigned src_w, uint64_t x)
{
    begin();
    uam_cpu_reg_write(&cpu, 8, 64, x);
    uam_ref s = uam_get(&B, src_w, 8);
    uam_ref r = uam_unop(&B, op, w, s);
    put_result(r, w);
    run(finish_checked());
    return reg64(0);
}

/* ---- semantics --------------------------------------------------------- */

static void test_arith(void)
{
    CHECK(bin(UAM_OP_ADD, 8, 0xFF, 1) == 0, "add wraps at 8 bits");
    CHECK(bin(UAM_OP_SUB, 32, 0, 1) == 0xFFFFFFFF, "sub wraps");
    CHECK(bin(UAM_OP_MUL, 16, 0x100, 0x100) == 0, "mul wraps");
    CHECK(bin(UAM_OP_MULHU, 32, 0xFFFFFFFF, 0xFFFFFFFF) == 0xFFFFFFFE, "mulhu 32");
    CHECK(bin(UAM_OP_MULHS, 32, 0xFFFFFFFF, 0xFFFFFFFF) == 0, "mulhs -1*-1");
    CHECK(bin(UAM_OP_MULHS, 32, 0x80000000, 2) == 0xFFFFFFFF, "mulhs min*2");
    CHECK(bin(UAM_OP_MULHU, 64, UINT64_C(1) << 63, 4) == 2, "mulhu 64");
    CHECK(bin(UAM_OP_MULHS, 64, ~UINT64_C(0), 5) == ~UINT64_C(0), "mulhs 64 -1*5");

    /* Division edge cases are defined (section 7.1). */
    CHECK(bin(UAM_OP_UDIV, 32, 7, 0) == 0xFFFFFFFF, "udiv by zero = all ones");
    CHECK(bin(UAM_OP_UREM, 32, 7, 0) == 7, "urem by zero = dividend");
    CHECK(bin(UAM_OP_SDIV, 32, 7, 0) == 0xFFFFFFFF, "sdiv by zero = -1");
    CHECK(bin(UAM_OP_SDIV, 32, 0x80000000, 0xFFFFFFFF) == 0x80000000, "sdiv MIN/-1 = MIN");
    CHECK(bin(UAM_OP_SDIV, 64, UINT64_C(1) << 63, ~UINT64_C(0)) == UINT64_C(1) << 63, "sdiv64 MIN/-1");
    CHECK(bin(UAM_OP_SREM, 32, 0x80000000, 0xFFFFFFFF) == 0, "srem MIN/-1 = 0");
    CHECK(bin(UAM_OP_SREM, 32, (uint32_t)-7, 2) == (uint32_t)-1, "srem takes dividend sign");
    CHECK(bin(UAM_OP_SDIV, 32, (uint32_t)-7, 2) == (uint32_t)-3, "sdiv truncates toward zero");
    CHECK(bin(UAM_OP_SREM, 32, 7, 0) == 7, "srem by zero = dividend");
}

static void test_logic_shift(void)
{
    CHECK(bin(UAM_OP_AND, 1, 1, 1) == 1 && bin(UAM_OP_XOR, 1, 1, 1) == 0, "boolean logic at W=1");
    CHECK(bin(UAM_OP_SHL, 32, 1, 31) == 0x80000000, "shl");
    CHECK(bin(UAM_OP_SHL, 32, 1, 32) == 0, "shl by W = 0 (no masking)");
    CHECK(bin(UAM_OP_LSHR, 32, 0x80000000, 40) == 0, "lshr by >W = 0");
    CHECK(bin(UAM_OP_ASHR, 32, 0x80000000, 33) == 0xFFFFFFFF, "ashr by >W = sign fill");
    CHECK(bin(UAM_OP_ASHR, 32, 0x40000000, 33) == 0, "ashr positive by >W = 0");
    CHECK(bin(UAM_OP_ASHR, 8, 0x80, 3) == 0xF0, "ashr 8-bit");
    CHECK(bin(UAM_OP_ROTR, 32, 1, 1) == 0x80000000, "rotr");
    CHECK(bin(UAM_OP_ROTR, 32, 1, 33) == 0x80000000, "rotr amount mod W");
    CHECK(bin(UAM_OP_ROTR, 16, 0x1234, 0) == 0x1234, "rotr by 0");
}

static void test_compare_flags(void)
{
    CHECK(bin(UAM_OP_SLT, 32, 0xFFFFFFFF, 0) == 1, "slt -1 < 0");
    CHECK(bin(UAM_OP_ULT, 32, 0xFFFFFFFF, 0) == 0, "ult");
    CHECK(bin(UAM_OP_SLE, 32, 5, 5) == 1 && bin(UAM_OP_ULE, 8, 6, 5) == 0, "le");
    CHECK(bin(UAM_OP_EQ, 64, 3, 3) == 1 && bin(UAM_OP_NE, 64, 3, 3) == 0, "eq/ne");
    CHECK(bin(UAM_OP_CARRY, 8, 0xFF, 1) == 1 && bin(UAM_OP_CARRY, 8, 0xFE, 1) == 0, "carry");
    CHECK(bin(UAM_OP_CARRY, 64, ~UINT64_C(0), 1) == 1, "carry 64");
    CHECK(bin(UAM_OP_SCARRY, 8, 0x7F, 1) == 1 && bin(UAM_OP_SCARRY, 8, 0x80, 0xFF) == 1, "scarry");
    CHECK(bin(UAM_OP_SCARRY, 8, 0x7E, 1) == 0, "no scarry");
    CHECK(bin(UAM_OP_SBORROW, 8, 0x80, 1) == 1 && bin(UAM_OP_SBORROW, 8, 0, 1) == 0, "sborrow");
}

static void test_bits_conv(void)
{
    CHECK(un(UAM_OP_CLZ, 32, 32, 0) == 32, "clz(0) = W");
    CHECK(un(UAM_OP_CLZ, 32, 32, 1) == 31, "clz(1)");
    CHECK(un(UAM_OP_CLZ, 8, 8, 0x10) == 3, "clz 8-bit");
    CHECK(un(UAM_OP_CTZ, 16, 16, 0) == 16, "ctz(0) = W");
    CHECK(un(UAM_OP_CTZ, 64, 64, UINT64_C(1) << 40) == 40, "ctz 64");
    CHECK(un(UAM_OP_POPCNT, 16, 16, 0xF0F0) == 8, "popcnt");
    CHECK(un(UAM_OP_BSWAP, 32, 32, 0x11223344) == 0x44332211, "bswap 32");
    CHECK(un(UAM_OP_BSWAP, 16, 16, 0xAABB) == 0xBBAA, "bswap 16");
    CHECK(un(UAM_OP_SEXT, 32, 8, 0x80) == 0xFFFFFF80, "sext 8->32");
    CHECK(un(UAM_OP_SEXT, 64, 32, 0x7FFFFFFF) == 0x7FFFFFFF, "sext positive");
    CHECK(un(UAM_OP_ZEXT, 64, 16, 0xFFFF) == 0xFFFF, "zext");
    CHECK(un(UAM_OP_TRUNC, 16, 32, 0x12345678) == 0x5678, "trunc");

    begin();
    uam_ref c = uam_binop(&B, UAM_OP_EQ, 32, uam_imm(&B, 1), uam_imm(&B, 1));
    put_result(uam_select(&B, 32, c, uam_imm(&B, 0xAA), uam_imm(&B, 0xBB)), 32);
    run(finish_checked());
    CHECK(reg64(0) == 0xAA, "select");
}

/* ---- memory, guards, faults, async stop -------------------------------- */

static void test_memory(void)
{
    /* LE store, BE load. */
    begin();
    uam_st(&B, 32, UAM_ACC_LE, uam_imm(&B, RAM_BASE), uam_imm(&B, 0x11223344), NONE);
    put_result(uam_ld(&B, 32, UAM_ACC_BE, uam_imm(&B, RAM_BASE), NONE), 32);
    run(finish_checked());
    CHECK(reg64(0) == 0x44332211, "LE store / BE load");

    /* AMO add returns the old value and updates memory. */
    begin();
    uam_ref old = uam_amo(&B, 32, UAM_ACC_LE, UAM_AMO_ADD, uam_imm(&B, RAM_BASE), uam_imm(&B, 0x11), NONE);
    uam_put(&B, 64, 8, uam_unop(&B, UAM_OP_ZEXT, 64, old), NONE);
    next_insn();
    put_result(uam_ld(&B, 32, UAM_ACC_LE, uam_imm(&B, RAM_BASE), NONE), 32);
    run(finish_checked());
    CHECK(reg64(8) == 0x11223344 && reg64(0) == 0x11223355, "amo add");

    /* AMO signed max. */
    begin();
    uam_amo(&B, 32, UAM_ACC_LE, UAM_AMO_SMAX, uam_imm(&B, RAM_BASE), uam_imm(&B, 0xFFFFFFFF), NONE);
    put_result(uam_ld(&B, 32, UAM_ACC_LE, uam_imm(&B, RAM_BASE), NONE), 32);
    run(finish_checked());
    CHECK(reg64(0) == 0x11223355, "amo smax keeps larger signed value");

    /* CAS: failing compare leaves memory alone, matching compare swaps. */
    begin();
    uam_cas(&B, 32, UAM_ACC_LE, uam_imm(&B, RAM_BASE), uam_imm(&B, 0), uam_imm(&B, 0xDEAD), NONE);
    uam_ref o2 = uam_cas(&B, 32, UAM_ACC_LE, uam_imm(&B, RAM_BASE), uam_imm(&B, 0x11223355),
                         uam_imm(&B, 0xBEEF), NONE);
    uam_put(&B, 64, 8, uam_unop(&B, UAM_OP_ZEXT, 64, o2), NONE);
    next_insn();
    put_result(uam_ld(&B, 32, UAM_ACC_LE, uam_imm(&B, RAM_BASE), NONE), 32);
    run(finish_checked());
    CHECK(reg64(0) == 0xBEEF && reg64(8) == 0x11223355, "cas");

    /* Exclusive pair: success, then failure (reservation consumed). */
    begin();
    uam_ld(&B, 32, UAM_ACC_LE | UAM_ACC_X, uam_imm(&B, RAM_BASE + 8), NONE);
    uam_ref ok1 = uam_stx(&B, 32, UAM_ACC_LE, uam_imm(&B, RAM_BASE + 8), uam_imm(&B, 1), NONE);
    uam_ref ok2 = uam_stx(&B, 32, UAM_ACC_LE, uam_imm(&B, RAM_BASE + 8), uam_imm(&B, 2), NONE);
    uam_put(&B, 64, 8, uam_unop(&B, UAM_OP_ZEXT, 64, ok1), NONE);
    uam_put(&B, 64, 16, uam_unop(&B, UAM_OP_ZEXT, 64, ok2), NONE);
    next_insn();
    put_result(uam_ld(&B, 32, UAM_ACC_LE, uam_imm(&B, RAM_BASE + 8), NONE), 32);
    run(finish_checked());
    CHECK(reg64(8) == 1 && reg64(16) == 0 && reg64(0) == 1, "ld.x / st.x");
}

static void test_guards(void)
{
    begin();
    uam_ref f = uam_binop(&B, UAM_OP_EQ, 32, uam_imm(&B, 0), uam_imm(&B, 1)); /* false */
    uam_st(&B, 32, UAM_ACC_LE, uam_imm(&B, RAM_BASE + 16), uam_imm(&B, 0x55), f);
    uam_ref v = uam_ld(&B, 32, UAM_ACC_LE, uam_imm(&B, 0xF0000000), f); /* would fault */
    uam_put(&B, 64, 8, uam_unop(&B, UAM_OP_ZEXT, 64, v), NONE);
    next_insn();
    put_result(uam_ld(&B, 32, UAM_ACC_LE, uam_imm(&B, RAM_BASE + 16), NONE), 32);
    uam_block_result r = run(finish_checked());
    CHECK(r.kind == UAM_BR_NEXT, "false-guarded load does not fault");
    CHECK(reg64(0) == 0, "false-guarded store has no effect");
    CHECK(reg64(8) == 0, "false-guarded op defines dst = 0");

    /* Guarded side exit. */
    begin();
    uam_ref t = uam_binop(&B, UAM_OP_EQ, 32, uam_imm(&B, 1), uam_imm(&B, 1));
    uam_exit(&B, t, uam_imm(&B, 0x200), UAM_EXIT_JUMP);
    uam_exit(&B, NONE, uam_imm(&B, 0x300), UAM_EXIT_JUMP);
    r = run(finish_checked());
    CHECK(r.kind == UAM_BR_NEXT && r.pc == 0x200, "taken side exit");
}

static void test_faults_and_stops(void)
{
    /* Fault in the second instruction: precise pc, first instruction committed. */
    begin();
    uam_put(&B, 64, 8, uam_imm(&B, 0x77), NONE);
    uam_imark(&B, 0x104, 4);
    uam_ref v = uam_ld(&B, 32, UAM_ACC_LE, uam_imm(&B, 0xF0000000), NONE);
    uam_put(&B, 64, 16, uam_unop(&B, UAM_OP_ZEXT, 64, v), NONE);
    uam_exit(&B, NONE, uam_imm(&B, 0x108), UAM_EXIT_JUMP);
    uam_block_result r = run(finish_checked());
    CHECK(r.kind == UAM_BR_FAULT && r.exc.kind == UAM_EXC_LOAD_FAULT, "load fault reported");
    CHECK(r.exc.pc == 0x104 && r.exc.tval == 0xF0000000, "fault pc/tval precise");
    CHECK(reg64(8) == 0x77 && reg64(16) == 0, "earlier insn committed, faulting insn not");

    /* Misaligned ALIGNED access. */
    begin();
    uam_ld(&B, 32, UAM_ACC_LE | UAM_ACC_ALIGNED, uam_imm(&B, RAM_BASE + 2), NONE);
    uam_exit(&B, NONE, uam_imm(&B, 0x104), UAM_EXIT_JUMP);
    r = run(finish_checked());
    CHECK(r.kind == UAM_BR_FAULT && r.exc.kind == UAM_EXC_LOAD_MISALIGNED, "aligned check");

    /* Exit request stops at the next IMARK, never before the first. */
    begin();
    uam_put(&B, 64, 8, uam_imm(&B, 1), NONE);
    uam_imark(&B, 0x104, 4);
    uam_put(&B, 64, 16, uam_imm(&B, 2), NONE);
    uam_exit(&B, NONE, uam_imm(&B, 0x108), UAM_EXIT_JUMP);
    uam_block *blk = finish_checked();
    cpu.exit_request = true; /* begin() cleared it */
    r = run(blk);
    CHECK(r.kind == UAM_BR_INTERRUPTED && r.pc == 0x104, "interrupted at instruction boundary");
    CHECK(reg64(8) == 1 && reg64(16) == 0 && cpu.icount == 1, "first insn completed only");

    /* Traps and waits are terminators carrying their operands. */
    begin();
    uam_trap(&B, NONE, UAM_TRAP_HALT, uam_imm(&B, 42), uam_imm(&B, 0));
    r = run(finish_checked());
    CHECK(r.kind == UAM_BR_TRAP && r.trap_class == UAM_TRAP_HALT && r.exc.cause == 42, "trap");
}

/* ---- verifier ---------------------------------------------------------- */

static bool verifies(uam_block *blk, const char *expect_substr)
{
    char err[256];
    uam_verify_ctx vc = { .regfile_size = REGFILE };
    bool ok = uam_verify(blk, &vc, err, sizeof(err));
    if (!ok && expect_substr && !strstr(err, expect_substr))
        fprintf(stderr, "  (verifier said: %s)\n", err);
    uam_block_free(blk);
    return ok;
}

static void test_verifier(void)
{
    /* Missing terminator. */
    begin();
    uam_put(&B, 64, 0, uam_imm(&B, 1), NONE);
    CHECK(!verifies(uam_builder_finish(&B), "end"), "missing terminator rejected");

    /* Commit rule: PUT before a may-fault op in the same instruction. */
    begin();
    uam_put(&B, 64, 0, uam_imm(&B, 1), NONE);
    uam_ld(&B, 32, UAM_ACC_LE, uam_imm(&B, RAM_BASE), NONE);
    uam_exit(&B, NONE, uam_imm(&B, 0), UAM_EXIT_JUMP);
    CHECK(!verifies(uam_builder_finish(&B), "commit"), "commit rule enforced");

    /* ...but the same order across an instruction boundary is fine. */
    begin();
    uam_put(&B, 64, 0, uam_imm(&B, 1), NONE);
    uam_imark(&B, 0x104, 4);
    uam_ld(&B, 32, UAM_ACC_LE, uam_imm(&B, RAM_BASE), NONE);
    uam_exit(&B, NONE, uam_imm(&B, 0), UAM_EXIT_JUMP);
    CHECK(verifies(uam_builder_finish(&B), NULL), "commit rule is per instruction");

    /* Width mismatch. */
    begin();
    uam_ref x = uam_get(&B, 32, 0);
    uam_binop(&B, UAM_OP_ADD, 64, x, uam_imm(&B, 1));
    uam_exit(&B, NONE, uam_imm(&B, 0), UAM_EXIT_JUMP);
    CHECK(!verifies(uam_builder_finish(&B), "width"), "operand width mismatch rejected");

    /* Immediate too wide. */
    begin();
    uam_binop(&B, UAM_OP_ADD, 8, uam_imm(&B, 0x100), uam_imm(&B, 1));
    uam_exit(&B, NONE, uam_imm(&B, 0), UAM_EXIT_JUMP);
    CHECK(!verifies(uam_builder_finish(&B), "fit"), "oversized immediate rejected");

    /* Guard on a pure op. */
    begin();
    uam_ref g = uam_binop(&B, UAM_OP_EQ, 32, uam_imm(&B, 1), uam_imm(&B, 1));
    uam_emit(&B, UAM_OP_ADD, 32, 0, 0, g, uam_imm(&B, 1), uam_imm(&B, 2), NONE, 32);
    uam_exit(&B, NONE, uam_imm(&B, 0), UAM_EXIT_JUMP);
    CHECK(!verifies(uam_builder_finish(&B), "guard"), "guard on pure op rejected");

    /* Use before definition (temp allocated but never defined). */
    begin();
    uam_ref ghost = uam_builder_new_temp(&B, 32);
    uam_binop(&B, UAM_OP_ADD, 32, ghost, uam_imm(&B, 1));
    uam_exit(&B, NONE, uam_imm(&B, 0), UAM_EXIT_JUMP);
    CHECK(!verifies(uam_builder_finish(&B), "before"), "use before definition rejected");

    /* Memory op without byte order. */
    begin();
    uam_ld(&B, 32, 0, uam_imm(&B, RAM_BASE), NONE);
    uam_exit(&B, NONE, uam_imm(&B, 0), UAM_EXIT_JUMP);
    CHECK(!verifies(uam_builder_finish(&B), "LE/BE"), "byte order required");

    /* Conversion in the wrong direction. */
    begin();
    uam_ref y = uam_get(&B, 32, 0);
    uam_unop(&B, UAM_OP_ZEXT, 16, y);
    uam_exit(&B, NONE, uam_imm(&B, 0), UAM_EXIT_JUMP);
    CHECK(!verifies(uam_builder_finish(&B), "conversion"), "bad conversion rejected");

    /* Register out of range. */
    begin();
    uam_get(&B, 64, REGFILE - 4);
    uam_exit(&B, NONE, uam_imm(&B, 0), UAM_EXIT_JUMP);
    CHECK(!verifies(uam_builder_finish(&B), "range"), "register bounds enforced");
}

static void test_printer(void)
{
    begin();
    uam_ref x = uam_get(&B, 32, 0);
    uam_put(&B, 32, 4, uam_binop(&B, UAM_OP_ADD, 32, x, uam_imm(&B, 0x10)), NONE);
    uam_exit(&B, NONE, uam_imm(&B, 0x104), UAM_EXIT_JUMP);
    uam_block *blk = finish_checked();
    char *buf = NULL;
    size_t len = 0;
    FILE *f = open_memstream(&buf, &len);
    static const uam_regdesc regs[] = { { "a0", 0, 32, UAM_REG_GPR }, { "a1", 4, 32, UAM_REG_GPR } };
    uam_print_ctx pc = { .regs = regs, .n_regs = 2 };
    uam_print_block(f, blk, &pc);
    fclose(f);
    CHECK(strstr(buf, "add.32 t0, 0x10") && strstr(buf, "{a1}") && strstr(buf, "exit.32 0x104 {jump}"),
          "printer output:\n%s", buf);
    free(buf);
    uam_block_free(blk);
}

int main(void)
{
    setup();
    test_arith();
    test_logic_shift();
    test_compare_flags();
    test_bits_conv();
    test_memory();
    test_guards();
    test_faults_and_stops();
    test_verifier();
    test_printer();
    uam_builder_free(&B);
    uam_bus_fini(&bus);
    printf("test_ir: %d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
