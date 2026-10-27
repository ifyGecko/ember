/*
 * runner.c - The stock command-line runner (uam/runner.h).
 *
 * Exit status:
 *   finisher/halt status (0 = pass) when the firmware ends the simulation,
 *   124 instruction or virtual-time limit, 125 deadlock, 126 internal error or
 *   fault loop, 127 debug trap or breakpoint, 128 livelock (waiting while timers
 *   keep firing, and no console or network input that could wake the hart),
 *   130 interrupted (SIGINT/SIGTERM),
 *   2 usage or setup error.
 * With --fuzz-replay, the most severe verdict of any input instead (uam/fuzz.h):
 *   0 ok, 20 crash (fault), 21 crash (oracle), 22 hang, 23 timeout, 24 rejected (the
 *   input route refused it); of several crashes, the first one's.
 * A fuzz run that stops on an error (an internal error of the executor, a lost
 * frontend) exits 2.
 */
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "uam/boards.h"
#include "uam/fuzz.h"
#include "uam/gdbstub.h"
#include "uam/host.h"
#include "uam/loader.h"
#include "uam/machine.h"
#include "uam/runner.h"
#include "uam/snapshot.h"

#define MAX_LIST 64

/* A growable list of addresses (--log-pc, --break): no limit on how many are given. */
typedef struct pc_list {
    uint64_t *pc;
    unsigned  n, cap;
} pc_list;

static bool pc_list_add(pc_list *l, uint64_t pc)
{
    if (l->n == l->cap) {
        unsigned ncap = l->cap ? l->cap * 2 : 16;
        uint64_t *np = realloc(l->pc, sizeof(*np) * ncap);
        if (!np)
            return false;
        l->pc = np;
        l->cap = ncap;
    }
    l->pc[l->n++] = pc;
    return true;
}

/* --dump-mem: guest memory shown or saved when the run stops. */
typedef struct mem_dump {
    uint64_t    addr, len;
    bool        phys;
    const char *file;   /* raw bytes to this file; NULL: hex dump on stderr */
} mem_dump;

/* [p:]ADDR+LEN[=FILE]; LEN takes K/M/G suffixes. */
static bool parse_mem_dump(char *s, mem_dump *d)
{
    char *end;
    *d = (mem_dump){ 0 };
    if (!strncmp(s, "p:", 2)) {
        d->phys = true;
        s += 2;
    }
    d->addr = strtoull(s, &end, 0);
    if (end == s || *end != '+')
        return false;
    s = end + 1;
    d->len = strtoull(s, &end, 0);
    if (end == s)
        return false;
    switch (*end) {
    case 'k': case 'K': d->len <<= 10; end++; break;
    case 'm': case 'M': d->len <<= 20; end++; break;
    case 'g': case 'G': d->len <<= 30; end++; break;
    default: break;
    }
    if (*end == '=' && end[1])
        d->file = end + 1;
    else if (*end)
        return false;
    return d->len > 0 && d->len <= (UINT64_C(1) << 30);
}

/* Read d's bytes the way a debugger does (no device side effects). Returns how many
 * leading bytes were readable; if not all, `why` says what stopped it. */
static uint64_t read_mem_dump(uam_machine *m, const mem_dump *d, uint8_t *buf, char *why,
                              size_t why_len)
{
    uam_cpu *cpu = uam_machine_cpu(m, 0);
    for (uint64_t i = 0; i < d->len; i++) {
        uint64_t pa = d->addr + i, b;
        if (!d->phys && cpu->isa->translate) {
            uam_xlate xl = { 0 };
            if (cpu->isa->translate(cpu, d->addr + i, UAM_XLATE_DEBUG, &xl) != UAM_OK) {
                snprintf(why, why_len, "processor 0 does not map this virtual address");
                return i;
            }
            pa = xl.paddr;
        }
        if (uam_bus_read(&m->bus, pa, 1, UAM_ACC_LE | UAM_ACC_DEBUG, &b) != UAM_OK) {
            const uam_region *r = uam_bus_find(&m->bus, pa);
            if (!r)
                snprintf(why, why_len, "nothing is mapped at physical 0x%llx", (unsigned long long)pa);
            else if (r->kind == UAM_REGION_MMIO && r->dev && !r->dev->ops->peek)
                snprintf(why, why_len, "physical 0x%llx is device region '%s', whose device (%s) has "
                         "no side-effect-free read (uam_device_ops.peek)", (unsigned long long)pa,
                         r->name, r->dev->ops->type);
            else
                snprintf(why, why_len, "region '%s' refused a debug read at physical 0x%llx",
                         r->name, (unsigned long long)pa);
            return i;
        }
        buf[i] = (uint8_t)b;
    }
    return d->len;
}

static bool dump_memory(uam_machine *m, const mem_dump *d)
{
    uint8_t *buf = malloc(d->len);
    if (!buf) {
        fprintf(stderr, "uam-run: --dump-mem: out of memory\n");
        return false;
    }
    char why[256] = "";
    uint64_t got = read_mem_dump(m, d, buf, why, sizeof(why));
    const char *space = d->phys ? "physical" : "virtual";
    if (got < d->len)
        fprintf(stderr, "uam-run: --dump-mem: %s 0x%llx is not readable (%s); %llu of %llu bytes%s\n",
                space, (unsigned long long)(d->addr + got), why, (unsigned long long)got,
                (unsigned long long)d->len, got == 0 && d->file ? ", nothing written" : "");
    bool ok = got > 0;
    if (d->file && got) {
        FILE *f = fopen(d->file, "wb");
        ok = f && fwrite(buf, 1, got, f) == got;
        if (f && fclose(f) != 0)
            ok = false;
        if (ok)
            fprintf(stderr, "uam-run: %s memory 0x%llx+0x%llx written to %s\n", space,
                    (unsigned long long)d->addr, (unsigned long long)got, d->file);
        else
            fprintf(stderr, "uam-run: cannot write the memory dump to %s\n", d->file);
    } else if (got) {
        fprintf(stderr, "uam-run: %s memory 0x%llx+0x%llx:\n", space, (unsigned long long)d->addr,
                (unsigned long long)got);
        for (uint64_t i = 0; i < got; i += 16) {
            char hex[16 * 3 + 1] = "", asc[17] = "";
            for (unsigned k = 0; k < 16 && i + k < got; k++) {
                uint8_t c = buf[i + k];
                snprintf(hex + 3 * k, 4, "%02x ", c);
                asc[k] = c >= 0x20 && c < 0x7F ? (char)c : '.';
            }
            fprintf(stderr, "  %016llx  %-48s |%s|\n", (unsigned long long)(d->addr + i), hex, asc);
        }
    }
    free(buf);
    return ok;
}

/* Whole file into a new buffer. */
static bool read_file(const char *path, uint8_t **data, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    uint8_t *buf = NULL;
    size_t n = 0, cap = 0;
    for (;;) {
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 4096;
            uint8_t *nb = realloc(buf, ncap);
            if (!nb) {
                free(buf);
                fclose(f);
                return false;
            }
            buf = nb;
            cap = ncap;
        }
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (got == 0)
            break;
    }
    bool ok = !ferror(f);
    fclose(f);
    if (!ok) {
        free(buf);
        return false;
    }
    *data = buf;
    *len = n;
    return true;
}

static void on_mmio_input_exhausted(void *ctx)
{
    fprintf(stderr, "uam-run: --mmio-input used up after %zu bytes; 'input' registers now read 0\n",
            uam_mmio_input_pos(ctx));
}

/* The extensions of the current run, for usage(). */
static const uam_runner_ext *const *g_exts;
static size_t g_n_exts;

static void usage(FILE *out)
{
    fprintf(out,
        "usage: uam-run [options] [IMAGE]\n"
        "  -b, --board NAME        board to build (default: rv32-ref); --board list shows all\n"
        "      --raw-base ADDR     load address for raw (non-ELF) images (default: board reset pc)\n"
        "      --entry PC          start the hart at PC (default: the ELF entry, or the raw\n"
        "                          image's load address)\n"
        "      --image NAME=PATH   named image for the board to place (repeatable)\n"
        "  -o, --option KEY=VALUE  board-specific option (repeatable)\n"
        "  -n, --max-insns N       stop after N guest instructions, counted from where the run\n"
        "                          starts (after --load-snapshot: from the snapshot; 0 = unlimited)\n"
        "      --max-ticks N       stop after N ticks of virtual time from where the run starts,\n"
        "                          idle time included (0 = unlimited; exit 124 like --max-insns)\n"
        "      --max-stall N       after N consecutive timer events with no instruction\n"
        "                          retired (e.g. WAIT whose interrupt is not wired), wait\n"
        "                          for console/network input instead of skipping time; with\n"
        "                          no input source, stop (exit 128, livelock)\n"
        "                          (default 4194304; 0 = never)\n"
        "      --quantum N         with several processors, run each N instructions at a\n"
        "                          time before the next (default 4096)\n"
        "      --ram SIZE          RAM size in bytes (suffix K/M/G allowed)\n"
        "      --unknown-mmio P    unmapped accesses: fault (default), zero, echo\n"
        "      --mmio-rule RULE    answer unmodelled registers (repeatable; later rules win):\n"
        "                          ADDR[+SIZE]=POLICY[,width=N][,set=MASK][,clear=MASK]\n"
        "                          POLICY: zero, echo, fixed:VALUE, fault, input\n"
        "      --mmio-input FILE   bytes read by 'input' rules, in order (0 once used up)\n"
        "      --console SPEC      console backend: stdio (default), stdio-raw, null, pty,\n"
        "                          tcp:[HOST:]PORT, unix:PATH, file:OUT[,in=FILE], string:TEXT\n"
        "      --uart-input STR    shorthand for --console string:STR\n"
        "      --nic PORT=SPEC     attach the board's network port PORT (repeatable; port names\n"
        "                          are the board's) to a backend: tap:[IFNAME], tun:[IFNAME],\n"
        "                          tap-fd:FD, tun-fd:FD, udp:LPORT:RHOST:RPORT, null;\n"
        "                          append ,pcap=FILE to capture. Ports left out are unplugged\n"
        "      --realtime[=TPS]    pace virtual time to the host clock (TPS ticks/s, default 100M);\n"
        "                          implied by a --nic with a host interface\n"
        "      --virtual-time      keep virtual pacing even with host network interfaces\n"
        "      --block-insns N     max guest instructions per block (default 32)\n"
        "  -t, --trace LIST        comma list of: blocks, ir, exc (blocks: every block executed,\n"
        "                          also with --jit, which does not chain blocks while tracing)\n"
        "      --trace-range LO:HI trace blocks only for pcs in [LO, HI)\n"
        "      --trace-after N     trace blocks only once the instruction count reaches N. N is\n"
        "                          absolute, like every icount the runner prints (a snapshot\n"
        "                          keeps the count); +N: N instructions after the run starts\n"
        "      --log-pc ADDR       log each arrival at ADDR (repeatable)\n"
        "      --break ADDR        stop when ADDR is reached (repeatable)\n"
        "      --dump-mem [p:]ADDR+LEN[=FILE]  when the run stops (--break, -n, ...), hex-dump\n"
        "                          LEN bytes of guest memory at ADDR to stderr, or write them\n"
        "                          raw to FILE (repeatable). ADDR is virtual, translated as\n"
        "                          processor 0 sees it (as gdb does; x86: linear); p: physical\n"
        "      --gdb PORT          wait for a gdb connection on localhost:PORT\n"
        "      --load-snapshot FILE  start from a snapshot instead of the reset state (same\n"
        "                          board and options; IMAGE is not needed)\n"
        "      --save-snapshot FILE  save a snapshot when the run stops (-n, --max-ticks,\n"
        "                          --break, SIGINT, ...), unless --snapshot-at saved one\n"
        "      --snapshot-at ADDR  save the --save-snapshot FILE when ADDR is first reached,\n"
        "                          then keep running (add --break ADDR to stop there)\n"
        "      --map               print the bus map after building the board\n"
        "      --no-verify         skip IR verification of translated blocks\n"
        "      --no-opt            do not optimize translated blocks\n"
        "      --jit               compile blocks to native code (x86-64 host)\n"
        "      --no-chain          with --jit, return to the run loop after every block\n"
        "      --perf-map          with --jit, write /tmp/perf-PID.map so perf names compiled\n"
        "                          blocks by guest pc (env UAM_PERF_MAP=1)\n"
        "      --jit-opmap FILE    with --jit, append which IR instruction each piece of\n"
        "                          native code came from to FILE, for tools/jitprof.py (env\n"
        "                          UAM_JIT_OPMAP=FILE)\n"
        "      --cov-blocks        count edges between translated blocks (approximate: blocks\n"
        "                          are not basic blocks) in a coverage map\n"
        "      --cov-map-size N    coverage map bytes, a power of two (default 64K)\n"
        "      --cov-out FILE      write the raw coverage map to FILE when the run stops\n"
        "                          (fuzzing: the last input's map)\n"
        "      --fuzz-begin PC     fuzz: boot to PC, snapshot there, run each input from it\n"
        "                          (repeatable: the snapshot is at the first one reached, and\n"
        "                          any of them begins an iteration)\n"
        "      --fuzz-end PC       an iteration ends at PC (repeatable: any of them; default:\n"
        "                          when a --fuzz-begin PC is reached again)\n"
        "      --fuzz-begin-skip N boot passes the begin PC N times and snapshots at the next\n"
        "                          arrival (firmware that runs its handler at start-up)\n"
        "      --fuzz-input ROUTE  how inputs reach the guest: mem:ADDR[,len=ADDR][,max=N]\n"
        "                          [,reject] (reject: refuse longer inputs, verdict rejected),\n"
        "                          mmio (--mmio-rule input registers), or a board input\n"
        "                          (stock boards: uart0)\n"
        "      --fuzz-crash-pc PC  reaching PC is a crash (repeatable)\n"
        "      --fuzz-crash-exc L  exceptions that are crashes: comma list of fetch, load,\n"
        "                          store, align, xlate, trap, sysreg, all, none\n"
        "                          (default fetch,load,store,align)\n"
        "      --fuzz-crash-irq N  an interrupt from line N (as the board wires it to the\n"
        "                          CPU) is a crash (repeatable)\n"
        "      --fuzz-iter-insns N instruction budget per input (default 10000000; timeout)\n"
        "      --fuzz-replay PATH  run each input in FILE or DIR once and print the verdict;\n"
        "                          -n bounds the boot; exit 0 ok, 20 fault, 21 oracle,\n"
        "                          22 hang, 23 timeout, 24 rejected (the most severe of\n"
        "                          any input: a crash, a hang, a timeout, a rejection)\n"
        "  -s, --stats             print execution statistics at exit\n"
        "  -d, --dump-state        print hart state at exit (every processor's)\n"
        "  -v, --verbose           more logging (repeatable)\n"
        "  -h, --help\n");
    for (size_t i = 0; i < g_n_exts; i++)
        if (g_exts[i]->help)
            fputs(g_exts[i]->help, out);
}

static uint64_t parse_size(const char *s)
{
    char *end;
    uint64_t v = strtoull(s, &end, 0);
    switch (*end) {
    case 'k': case 'K': v <<= 10; break;
    case 'm': case 'M': v <<= 20; break;
    case 'g': case 'G': v <<= 30; break;
    default: break;
    }
    return v;
}

static unsigned parse_trace(const char *s)
{
    unsigned t = 0;
    char *copy = strdup(s);
    for (char *tok = strtok(copy, ","); tok; tok = strtok(NULL, ",")) {
        if (!strcmp(tok, "blocks"))   t |= UAM_TRACE_BLOCKS;
        else if (!strcmp(tok, "ir"))  t |= UAM_TRACE_IR;
        else if (!strcmp(tok, "exc")) t |= UAM_TRACE_EXC;
        else fprintf(stderr, "uam-run: unknown trace flag '%s'\n", tok);
    }
    free(copy);
    return t;
}

/* Split "KEY=VALUE" in place. */
static bool split_kv(char *s, const char **k, const char **v)
{
    char *eq = strchr(s, '=');
    if (!eq || eq == s)
        return false;
    *eq = 0;
    *k = s;
    *v = eq + 1;
    return true;
}

/* ---- signals and console quit -------------------------------------------- */

static uam_machine *volatile g_machine;
static uam_fuzz *volatile g_fuzz;

static void on_signal(int sig)
{
    (void)sig;
    if (g_fuzz)
        uam_fuzz_abort(g_fuzz);
    else if (g_machine)
        uam_exec_stop(&g_machine->exec, 130);
}

static void on_console_quit(void *ctx)
{
    uam_exec_stop(&((uam_machine *)ctx)->exec, 0);
}

/* ---- PC hooks from the command line --------------------------------------- */

/* " cpu N" for processors other than 0, as the trace marks them. */
static const char *cpu_tag(const uam_cpu *cpu, char *buf, size_t n)
{
    if (cpu->hartid)
        snprintf(buf, n, ", cpu %u", cpu->hartid);
    else
        buf[0] = '\0';
    return buf;
}

static uam_hook_action log_pc_hook(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    uint64_t *hits = ctx;
    char tag[24];
    (*hits)++;
    fprintf(stderr, "uam-run: pc 0x%llx reached (hit %llu, icount %llu%s)\n", (unsigned long long)pc,
            (unsigned long long)*hits, (unsigned long long)cpu->icount, cpu_tag(cpu, tag, sizeof(tag)));
    return UAM_HOOK_CONTINUE;
}

/* --snapshot-at: save once, on the first arrival. */
typedef struct snap_at {
    uam_machine *m;
    const char  *path;
    bool         done;
} snap_at;

static uam_hook_action snapshot_at_hook(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    snap_at *sa = ctx;
    if (sa->done)
        return UAM_HOOK_CONTINUE;
    sa->done = true;
    char tag[24];
    if (uam_machine_save_snapshot(sa->m, sa->path) == UAM_OK)
        fprintf(stderr, "uam-run: snapshot saved to %s at pc 0x%llx (icount %llu%s)\n", sa->path,
                (unsigned long long)pc, (unsigned long long)cpu->icount,
                cpu_tag(cpu, tag, sizeof(tag)));
    else
        fprintf(stderr, "uam-run: saving a snapshot to %s failed\n", sa->path);
    return UAM_HOOK_CONTINUE;
}

static uam_hook_action break_hook(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    (void)ctx;
    char tag[24];
    fprintf(stderr, "uam-run: breakpoint at 0x%llx (icount %llu%s)\n", (unsigned long long)pc,
            (unsigned long long)cpu->icount, cpu_tag(cpu, tag, sizeof(tag)));
    return UAM_HOOK_STOP;
}

/* --fuzz-begin: the harness with the replay frontend. Returns the exit status. */
/* The extensions of a run and their states. */
typedef struct ext_set {
    const uam_runner_ext *const *ext;
    void                **st;
    size_t                n;
} ext_set;

static int run_fuzz(uam_machine *m, const uam_fuzz_config *fz, bool cov_blocks, const char *route,
                    uam_mmio_input *mmio_input, const char *replay, const char *cov_out,
                    uint64_t map_size, bool stats, const uam_runner_setup *xs, const ext_set *es)
{
    char err[512];
    uam_fuzz_config cfg = *fz;
    cfg.map_size = (uint32_t)map_size;
    uam_fuzz *f = uam_fuzz_create(m, &cfg, err, sizeof(err));
    if (!f) {
        fprintf(stderr, "uam-run: cannot set up fuzzing: %s\n", err);
        return 2;
    }
    int rc = 2;
    uam_fuzz_replay *r = NULL;
    if (cov_blocks && uam_fuzz_set_block_coverage(f) != UAM_OK) {
        fprintf(stderr, "uam-run: cannot enable coverage\n");
        goto out;
    }
    if (xs->sites) {
        if (uam_fuzz_set_sites(f, xs->sites, xs->n_sites, xs->scheme, err, sizeof(err)) != UAM_OK) {
            fprintf(stderr, "uam-run: coverage sites: %s\n", err);
            goto out;
        }
        if (xs->namer && m->exec.cov_pass)
            uam_cov_pass_set_namer(m->exec.cov_pass, xs->namer, xs->namer_ctx);
    }
    for (size_t i = 0; i < es->n; i++)
        if (es->ext[i]->attach && es->ext[i]->attach(es->st[i], m, f) != UAM_OK)
            goto out;
    if (mmio_input && uam_fuzz_add_route(f, "mmio", uam_fuzz_mmio_deliver, mmio_input) != UAM_OK)
        goto out;
    if (uam_fuzz_select_route(f, route, err, sizeof(err)) != UAM_OK) {
        fprintf(stderr, "uam-run: --fuzz-input: %s\n", err);
        goto out;
    }
    if (!xs->frontend && !(r = uam_fuzz_replay_create_ex(replay, f, stdout, xs->replay_sidecars,
                                                         xs->n_replay_sidecars)))
        goto out;
    g_fuzz = f;
    uam_status st = xs->frontend ? uam_fuzz_run(f, xs->frontend, xs->frontend_ctx)
                                 : uam_fuzz_run(f, &uam_fuzz_replay_frontend, r);
    g_fuzz = NULL;
    fflush(stdout);
    const uam_fuzz_stats *fs = uam_fuzz_get_stats(f);
    if (uam_fuzz_aborted(f)) {
        fprintf(stderr, "uam-run: fuzz: interrupted\n");
        rc = 130;
    } else if (st != UAM_OK) {
        fprintf(stderr, "uam-run: fuzz: stopped by an error after %llu inputs\n",
                (unsigned long long)fs->iterations);
    } else {
        rc = r ? uam_fuzz_replay_exit_status(r) : 0;
    }
    fprintf(stderr, "uam-run: fuzz: %llu inputs: %llu ok, %llu crash (fault), %llu crash (oracle), "
            "%llu hang, %llu timeout, %llu rejected\n", (unsigned long long)fs->iterations,
            (unsigned long long)fs->by_verdict[UAM_FUZZ_OK],
            (unsigned long long)fs->by_verdict[UAM_FUZZ_CRASH_FAULT],
            (unsigned long long)fs->by_verdict[UAM_FUZZ_CRASH_ORACLE],
            (unsigned long long)fs->by_verdict[UAM_FUZZ_HANG],
            (unsigned long long)fs->by_verdict[UAM_FUZZ_TIMEOUT],
            (unsigned long long)fs->by_verdict[UAM_FUZZ_REJECTED]);
    if (cov_out) {
        size_t n;
        const uint8_t *map = r ? uam_fuzz_replay_last_map(r, &n) : uam_fuzz_map(f, &n);
        FILE *fp = map ? fopen(cov_out, "wb") : NULL;
        bool ok = fp && fwrite(map, 1, n, fp) == n;
        if (fp && fclose(fp) != 0)
            ok = false;
        if (!ok) {
            fprintf(stderr, "uam-run: cannot write the coverage map to %s\n", cov_out);
            if (rc == 0)
                rc = 2;
        }
    }
    if (stats) {
        size_t n;
        const uint8_t *map = uam_fuzz_map(f, &n);
        size_t hit = 0;
        for (size_t i = 0; i < n; i++)
            hit += map[i] != 0;
        fprintf(stderr, "stats: coverage map=%zu bytes, buckets hit by the last input=%zu\n", n, hit);
    }
out:
    uam_fuzz_replay_destroy(r);
    uam_fuzz_destroy(f);
    return rc;
}

static int run_main(int argc, char **argv, const ext_set *es)
{
    enum { OPT_RAW_BASE = 256, OPT_RAM, OPT_UNKNOWN, OPT_UART_IN, OPT_NO_VERIFY, OPT_NO_OPT, OPT_JIT, OPT_NO_CHAIN, OPT_BLOCK_INSNS,
           OPT_IMAGE, OPT_CONSOLE, OPT_REALTIME, OPT_TRACE_RANGE, OPT_TRACE_AFTER, OPT_LOG_PC,
           OPT_BREAK, OPT_GDB, OPT_MAP, OPT_NIC, OPT_VIRTUAL_TIME, OPT_MAX_TICKS, OPT_MAX_STALL, OPT_QUANTUM,
           OPT_LOAD_SNAP, OPT_SAVE_SNAP, OPT_SNAP_AT, OPT_MMIO_RULE, OPT_MMIO_INPUT,
           OPT_COV_BLOCKS, OPT_COV_OUT, OPT_COV_MAP_SIZE,
           OPT_FUZZ_BEGIN, OPT_FUZZ_END, OPT_FUZZ_INPUT, OPT_FUZZ_CRASH_PC, OPT_FUZZ_CRASH_EXC,
           OPT_FUZZ_ITER_INSNS, OPT_FUZZ_REPLAY, OPT_FUZZ_CRASH_IRQ, OPT_ENTRY, OPT_FUZZ_BEGIN_SKIP,
           OPT_DUMP_MEM, OPT_PERF_MAP, OPT_JIT_OPMAP, OPT_EXT_BASE = 1000 };
    static const struct option opts[] = {
        { "board", required_argument, NULL, 'b' },
        { "raw-base", required_argument, NULL, OPT_RAW_BASE },
        { "entry", required_argument, NULL, OPT_ENTRY },
        { "image", required_argument, NULL, OPT_IMAGE },
        { "option", required_argument, NULL, 'o' },
        { "max-insns", required_argument, NULL, 'n' },
        { "max-ticks", required_argument, NULL, OPT_MAX_TICKS },
        { "max-stall", required_argument, NULL, OPT_MAX_STALL },
        { "quantum", required_argument, NULL, OPT_QUANTUM },
        { "ram", required_argument, NULL, OPT_RAM },
        { "unknown-mmio", required_argument, NULL, OPT_UNKNOWN },
        { "mmio-rule", required_argument, NULL, OPT_MMIO_RULE },
        { "mmio-input", required_argument, NULL, OPT_MMIO_INPUT },
        { "console", required_argument, NULL, OPT_CONSOLE },
        { "uart-input", required_argument, NULL, OPT_UART_IN },
        { "realtime", optional_argument, NULL, OPT_REALTIME },
        { "nic", required_argument, NULL, OPT_NIC },
        { "virtual-time", no_argument, NULL, OPT_VIRTUAL_TIME },
        { "block-insns", required_argument, NULL, OPT_BLOCK_INSNS },
        { "trace", required_argument, NULL, 't' },
        { "trace-range", required_argument, NULL, OPT_TRACE_RANGE },
        { "trace-after", required_argument, NULL, OPT_TRACE_AFTER },
        { "log-pc", required_argument, NULL, OPT_LOG_PC },
        { "break", required_argument, NULL, OPT_BREAK },
        { "gdb", required_argument, NULL, OPT_GDB },
        { "load-snapshot", required_argument, NULL, OPT_LOAD_SNAP },
        { "save-snapshot", required_argument, NULL, OPT_SAVE_SNAP },
        { "snapshot-at", required_argument, NULL, OPT_SNAP_AT },
        { "map", no_argument, NULL, OPT_MAP },
        { "dump-mem", required_argument, NULL, OPT_DUMP_MEM },
        { "no-verify", no_argument, NULL, OPT_NO_VERIFY },
        { "no-opt", no_argument, NULL, OPT_NO_OPT },
        { "jit", no_argument, NULL, OPT_JIT },
        { "no-chain", no_argument, NULL, OPT_NO_CHAIN },
        { "perf-map", no_argument, NULL, OPT_PERF_MAP },
        { "jit-opmap", required_argument, NULL, OPT_JIT_OPMAP },
        { "cov-blocks", no_argument, NULL, OPT_COV_BLOCKS },
        { "cov-out", required_argument, NULL, OPT_COV_OUT },
        { "cov-map-size", required_argument, NULL, OPT_COV_MAP_SIZE },
        { "fuzz-begin", required_argument, NULL, OPT_FUZZ_BEGIN },
        { "fuzz-end", required_argument, NULL, OPT_FUZZ_END },
        { "fuzz-input", required_argument, NULL, OPT_FUZZ_INPUT },
        { "fuzz-crash-pc", required_argument, NULL, OPT_FUZZ_CRASH_PC },
        { "fuzz-crash-exc", required_argument, NULL, OPT_FUZZ_CRASH_EXC },
        { "fuzz-crash-irq", required_argument, NULL, OPT_FUZZ_CRASH_IRQ },
        { "fuzz-iter-insns", required_argument, NULL, OPT_FUZZ_ITER_INSNS },
        { "fuzz-replay", required_argument, NULL, OPT_FUZZ_REPLAY },
        { "fuzz-begin-skip", required_argument, NULL, OPT_FUZZ_BEGIN_SKIP },
        { "stats", no_argument, NULL, 's' },
        { "dump-state", no_argument, NULL, 'd' },
        { "verbose", no_argument, NULL, 'v' },
        { "help", no_argument, NULL, 'h' },
        { 0 },
    };

    const char *board_name = "rv32-ref", *console_spec = "stdio", *gdb_port = NULL;
    const char *load_snap = NULL, *save_snap = NULL, *mmio_input_path = NULL;
    static uam_mmio_rule mmio_rules[UAM_MAX_MMIO_RULES];
    size_t n_mmio_rules = 0;
    bool mmio_input_rule = false;
    uint8_t *mmio_input_data = NULL;
    size_t mmio_input_len = 0;
    uam_mmio_input *mmio_input = NULL;
    uint64_t snap_at_pc = 0;
    bool have_snap_at = false;
    char *uart_input_spec = NULL;
    uint64_t raw_base = 0, entry = 0, max_insns = 0, trace_lo = 0, trace_hi = 0, trace_after = 0;
    uint64_t max_ticks = 0, max_stall = UAM_EXEC_DEFAULT_MAX_STALL;
    uint32_t quantum = 0;
    uint64_t realtime_tps = 0, *log_hits = NULL;
    pc_list log_pcs = { 0 }, break_pcs = { 0 };
    mem_dump mem_dumps[MAX_LIST];
    unsigned n_mem_dumps = 0;
    bool trace_after_rel = false;
    bool have_raw_base = false, have_entry = false, stats = false, dump = false, verify = true, optimize = true, use_jit = false, chain = true, realtime = false;
    bool show_map = false, virtual_time = false, cov_blocks = false;
    const char *cov_out = NULL;
    uint64_t cov_map_size = 1u << 16;
    uint8_t *cov_map = NULL;
    uam_cov_state cov_state = { 0 };
    uam_fuzz_config fz = { .crash_exc = UAM_FUZZ_EXC_DEFAULT };
    uint64_t fuzz_crash_pcs[MAX_LIST], fuzz_end_pcs[MAX_LIST], fuzz_more_begins[MAX_LIST];
    size_t n_fuzz_end = 0, n_more_begins = 0;
    bool have_begin = false, fuzz_opts = false;
    const char *fuzz_input = NULL, *fuzz_replay = NULL;
    unsigned trace = 0, block_insns = 0;
    uam_board_option options[MAX_LIST];
    uam_board_image images[MAX_LIST];
    uam_board_netdev nics[MAX_LIST];
    const char *nic_specs[MAX_LIST];
    uam_board_config cfg = { .unknown_mmio = UAM_UNKNOWN_FAULT, .options = options, .images = images,
                             .netdevs = nics };

    /* The option table: ours, then each extension's. */
    enum { MAX_EXT_OPTS = 64 };
    struct option all[UAM_ARRAY_LEN(opts) + MAX_EXT_OPTS];
    size_t n_all = UAM_ARRAY_LEN(opts) - 1, n_ext_opts = 0;
    size_t ext_of[MAX_EXT_OPTS], opt_of[MAX_EXT_OPTS];
    memcpy(all, opts, sizeof(opts[0]) * n_all);
    for (size_t e = 0; e < es->n; e++) {
        for (size_t i = 0; i < es->ext[e]->n_opts; i++) {
            if (n_ext_opts == MAX_EXT_OPTS) {
                fprintf(stderr, "uam-run: too many extension options\n");
                return 2;
            }
            const uam_runner_opt *o = &es->ext[e]->opts[i];
            all[n_all++] = (struct option){ o->name, o->has_arg ? required_argument : no_argument,
                                            NULL, OPT_EXT_BASE + (int)n_ext_opts };
            ext_of[n_ext_opts] = e;
            opt_of[n_ext_opts++] = i;
        }
    }
    all[n_all] = (struct option){ 0 };
    bool cov_map_size_given = false;

    optind = 1;
    int c;
    while ((c = getopt_long(argc, argv, "b:n:t:o:sdvh", all, NULL)) != -1) {
        if (c >= OPT_EXT_BASE && (size_t)(c - OPT_EXT_BASE) < n_ext_opts) {
            size_t k = (size_t)(c - OPT_EXT_BASE), e = ext_of[k];
            char err[256] = "";
            if (!es->ext[e]->option(es->st[e], opt_of[k], optarg, err, sizeof(err))) {
                fprintf(stderr, "uam-run: --%s: %s\n", es->ext[e]->opts[opt_of[k]].name, err);
                return 2;
            }
            continue;
        }
        switch (c) {
        case 'b': board_name = optarg; break;
        case 'n': max_insns = strtoull(optarg, NULL, 0); break;
        case 't': trace = parse_trace(optarg); break;
        case 's': stats = true; break;
        case 'd': dump = true; break;
        case 'v': if (uam_log_threshold < UAM_LOG_DEBUG) uam_log_threshold++; break;
        case 'h': usage(stdout); return 0;
        case 'o':
            if (cfg.n_options == MAX_LIST
                || !split_kv(optarg, &options[cfg.n_options].key, &options[cfg.n_options].value)) {
                fprintf(stderr, "uam-run: bad option '%s' (expected KEY=VALUE)\n", optarg);
                return 2;
            }
            cfg.n_options++;
            break;
        case OPT_IMAGE:
            if (cfg.n_images == MAX_LIST
                || !split_kv(optarg, &images[cfg.n_images].name, &images[cfg.n_images].path)) {
                fprintf(stderr, "uam-run: bad --image '%s' (expected NAME=PATH)\n", optarg);
                return 2;
            }
            cfg.n_images++;
            break;
        case OPT_NIC: {
            const char *spec;
            if (cfg.n_netdevs == MAX_LIST || !split_kv(optarg, &nics[cfg.n_netdevs].name, &spec)) {
                fprintf(stderr, "uam-run: bad --nic '%s' (expected PORT=SPEC)\n", optarg);
                return 2;
            }
            for (size_t i = 0; i < cfg.n_netdevs; i++) {
                if (!strcmp(nics[i].name, nics[cfg.n_netdevs].name)) {
                    fprintf(stderr, "uam-run: port '%s' given twice\n", nics[i].name);
                    return 2;
                }
            }
            nic_specs[cfg.n_netdevs] = spec;
            nics[cfg.n_netdevs++].nd = NULL;
            break;
        }
        case OPT_VIRTUAL_TIME: virtual_time = true; break;
        case OPT_MAX_TICKS: max_ticks = strtoull(optarg, NULL, 0); break;
        case OPT_MAX_STALL: max_stall = strtoull(optarg, NULL, 0); break;
        case OPT_QUANTUM: quantum = (uint32_t)strtoul(optarg, NULL, 0); break;
        case OPT_RAW_BASE: raw_base = strtoull(optarg, NULL, 0); have_raw_base = true; break;
        case OPT_ENTRY: entry = strtoull(optarg, NULL, 0); have_entry = true; break;
        case OPT_RAM: cfg.ram_size = parse_size(optarg); break;
        case OPT_CONSOLE: console_spec = optarg; break;
        case OPT_UART_IN:
            free(uart_input_spec);
            uart_input_spec = malloc(strlen(optarg) + 8);
            if (uart_input_spec)
                sprintf(uart_input_spec, "string:%s", optarg);
            console_spec = uart_input_spec;
            break;
        case OPT_REALTIME:
            realtime = true;
            realtime_tps = optarg ? parse_size(optarg) : 0;
            break;
        case OPT_NO_VERIFY: verify = false; break;
        case OPT_NO_OPT: optimize = false; break;
        case OPT_JIT: use_jit = true; break;
        case OPT_NO_CHAIN: chain = false; break;
        /* read by the JIT when it is created (docs/jit-perf.md) */
        case OPT_PERF_MAP: setenv("UAM_PERF_MAP", "1", 1); break;
        case OPT_JIT_OPMAP: setenv("UAM_JIT_OPMAP", optarg, 1); break;
        case OPT_BLOCK_INSNS: block_insns = (unsigned)strtoul(optarg, NULL, 0); break;
        case OPT_TRACE_RANGE: {
            char *end;
            trace_lo = strtoull(optarg, &end, 0);
            trace_hi = *end == ':' ? strtoull(end + 1, NULL, 0) : 0;
            break;
        }
        case OPT_TRACE_AFTER:
            trace_after_rel = optarg[0] == '+';
            trace_after = strtoull(optarg + trace_after_rel, NULL, 0);
            break;
        case OPT_LOG_PC:
        case OPT_BREAK:
            if (!pc_list_add(c == OPT_LOG_PC ? &log_pcs : &break_pcs, strtoull(optarg, NULL, 0))) {
                fprintf(stderr, "uam-run: out of memory\n");
                return 2;
            }
            break;
        case OPT_GDB: gdb_port = optarg; break;
        case OPT_LOAD_SNAP: load_snap = optarg; break;
        case OPT_SAVE_SNAP: save_snap = optarg; break;
        case OPT_SNAP_AT: snap_at_pc = strtoull(optarg, NULL, 0); have_snap_at = true; break;
        case OPT_MAP: show_map = true; break;
        case OPT_DUMP_MEM:
            if (n_mem_dumps == MAX_LIST || !parse_mem_dump(optarg, &mem_dumps[n_mem_dumps])) {
                fprintf(stderr, "uam-run: bad --dump-mem '%s' (at most %d; [p:]ADDR+LEN[=FILE], "
                        "LEN up to 1G)\n", optarg, MAX_LIST);
                return 2;
            }
            n_mem_dumps++;
            break;
        case OPT_UNKNOWN:
            if (!strcmp(optarg, "fault"))     cfg.unknown_mmio = UAM_UNKNOWN_FAULT;
            else if (!strcmp(optarg, "zero")) cfg.unknown_mmio = UAM_UNKNOWN_ZERO;
            else if (!strcmp(optarg, "echo")) cfg.unknown_mmio = UAM_UNKNOWN_ECHO;
            else { fprintf(stderr, "uam-run: bad --unknown-mmio '%s'\n", optarg); return 2; }
            break;
        case OPT_MMIO_RULE: {
            char err[128];
            if (n_mmio_rules == UAM_MAX_MMIO_RULES) {
                fprintf(stderr, "uam-run: at most %u --mmio-rule options\n", UAM_MAX_MMIO_RULES);
                return 2;
            }
            if (uam_mmio_rule_parse(optarg, &mmio_rules[n_mmio_rules], err, sizeof(err)) != UAM_OK) {
                fprintf(stderr, "uam-run: bad --mmio-rule '%s': %s\n", optarg, err);
                return 2;
            }
            mmio_input_rule |= mmio_rules[n_mmio_rules].policy == UAM_MMIO_INPUT;
            n_mmio_rules++;
            break;
        }
        case OPT_MMIO_INPUT: mmio_input_path = optarg; break;
        case OPT_COV_BLOCKS: cov_blocks = true; break;
        case OPT_COV_OUT: cov_out = optarg; break;
        case OPT_COV_MAP_SIZE: cov_map_size = parse_size(optarg); cov_map_size_given = true; break;
        case OPT_FUZZ_BEGIN:
            /* Repeatable: the first is begin_pc, the others more begins. */
            if (!have_begin) {
                fz.begin_pc = strtoull(optarg, NULL, 0);
                have_begin = true;
            } else if (n_more_begins == MAX_LIST) {
                fprintf(stderr, "uam-run: at most %d --fuzz-begin options\n", MAX_LIST + 1);
                return 2;
            } else {
                fuzz_more_begins[n_more_begins++] = strtoull(optarg, NULL, 0);
            }
            break;
        case OPT_FUZZ_END:
            if (n_fuzz_end == MAX_LIST) {
                fprintf(stderr, "uam-run: at most %d --fuzz-end options\n", MAX_LIST);
                return 2;
            }
            fuzz_end_pcs[n_fuzz_end++] = strtoull(optarg, NULL, 0);
            fuzz_opts = true;
            break;
        case OPT_FUZZ_INPUT: fuzz_input = optarg; fuzz_opts = true; break;
        case OPT_FUZZ_CRASH_PC:
            if (fz.n_crash_pcs == MAX_LIST) {
                fprintf(stderr, "uam-run: at most %d --fuzz-crash-pc options\n", MAX_LIST);
                return 2;
            }
            fuzz_crash_pcs[fz.n_crash_pcs++] = strtoull(optarg, NULL, 0);
            fuzz_opts = true;
            break;
        case OPT_FUZZ_CRASH_EXC:
            if (!uam_fuzz_parse_exc_classes(optarg, &fz.crash_exc)) {
                fprintf(stderr, "uam-run: bad --fuzz-crash-exc '%s' (classes: fetch, load, store, "
                        "align, xlate, trap, sysreg, all, default, none)\n", optarg);
                return 2;
            }
            fuzz_opts = true;
            break;
        case OPT_FUZZ_CRASH_IRQ: {
            char *end;
            unsigned long line = strtoul(optarg, &end, 0);
            if (*end || line > 63) {
                fprintf(stderr, "uam-run: bad --fuzz-crash-irq '%s' (an interrupt line, 0..63)\n", optarg);
                return 2;
            }
            fz.crash_irq_lines |= UINT64_C(1) << line;
            fuzz_opts = true;
            break;
        }
        case OPT_FUZZ_ITER_INSNS: fz.iter_insns = strtoull(optarg, NULL, 0); fuzz_opts = true; break;
        case OPT_FUZZ_REPLAY: fuzz_replay = optarg; fuzz_opts = true; break;
        case OPT_FUZZ_BEGIN_SKIP: {
            char *end;
            unsigned long n = strtoul(optarg, &end, 0);
            if (*end || !*optarg || n > UINT32_MAX) {
                fprintf(stderr, "uam-run: bad --fuzz-begin-skip '%s' (a count)\n", optarg);
                return 2;
            }
            fz.begin_skip = (uint32_t)n;
            fuzz_opts = true;
            break;
        }
        default: usage(stderr); return 2;
        }
    }

    if (!strcmp(board_name, "list")) {
        uam_board_list(stdout);
        return 0;
    }
    if (optind + 1 < argc) {
        usage(stderr);
        return 2;
    }
    const char *image = optind < argc ? argv[optind] : NULL;
    if (have_snap_at && !save_snap) {
        fprintf(stderr, "uam-run: --snapshot-at needs --save-snapshot FILE\n");
        return 2;
    }
    unsigned provides = 0;
    for (size_t e = 0; e < es->n; e++)
        if (es->ext[e]->provides)
            provides |= es->ext[e]->provides(es->st[e]);
    const bool fuzzing = have_begin || (provides & UAM_RUNNER_EXT_BEGIN);
    if ((have_begin || n_fuzz_end) && (provides & UAM_RUNNER_EXT_BEGIN)) {
        fprintf(stderr, "uam-run: --fuzz-begin/--fuzz-end and a boundary file both set the "
                "iteration boundaries\n");
        return 2;
    }
    if (fuzz_opts && !fuzzing) {
        fprintf(stderr, "uam-run: the --fuzz-* options need --fuzz-begin PC (or a boundary file)\n");
        return 2;
    }
    if ((provides & UAM_RUNNER_EXT_FRONTEND) && !fuzzing) {
        fprintf(stderr, "uam-run: a fuzz frontend needs --fuzz-begin PC (or a boundary file)\n");
        return 2;
    }
    if ((provides & UAM_RUNNER_EXT_OBSERVER) && !fuzzing) {
        fprintf(stderr, "uam-run: a fuzz side channel needs --fuzz-begin PC (or a boundary file)\n");
        return 2;
    }
    if (fuzzing && !fuzz_replay && !(provides & UAM_RUNNER_EXT_FRONTEND)) {
        fprintf(stderr, "uam-run: fuzzing needs a frontend (--fuzz-replay FILE|DIR, ...)\n");
        return 2;
    }
    if (fuzz_replay && (provides & UAM_RUNNER_EXT_FRONTEND)) {
        fprintf(stderr, "uam-run: --fuzz-replay and another frontend are both given\n");
        return 2;
    }
    if (cov_blocks && (provides & UAM_RUNNER_EXT_SITES)) {
        fprintf(stderr, "uam-run: --cov-blocks and a coverage site list are both given\n");
        return 2;
    }
    if (fuzzing && !fuzz_input) {
        fprintf(stderr, "uam-run: --fuzz-begin needs an input route (--fuzz-input ROUTE)\n");
        return 2;
    }
    if (fuzzing && (gdb_port || save_snap)) {
        fprintf(stderr, "uam-run: --fuzz-begin does not combine with --gdb or --save-snapshot\n");
        return 2;
    }
    if (cov_out && !cov_blocks && !(provides & UAM_RUNNER_EXT_SITES) && !fuzzing) {
        fprintf(stderr, "uam-run: --cov-out needs a coverage source (--cov-blocks, a site list)\n");
        return 2;
    }
    fz.crash_pcs = fuzz_crash_pcs;
    fz.end_pcs = fuzz_end_pcs;
    fz.n_end_pcs = n_fuzz_end;
    fz.more_begin_pcs = fuzz_more_begins;
    fz.n_more_begin_pcs = n_more_begins;
    fz.boot_insns = max_insns;
    if (cov_map_size < 2 || cov_map_size > (UINT64_C(1) << 30) || (cov_map_size & (cov_map_size - 1))) {
        fprintf(stderr, "uam-run: --cov-map-size must be a power of two from 2 to 1G\n");
        return 2;
    }
    const uam_board *board = uam_board_find(board_name);
    if (!board) {
        fprintf(stderr, "uam-run: unknown board '%s'; available:\n", board_name);
        uam_board_list(stderr);
        return 2;
    }

    int rc = 2;
    uam_chardev *console = uam_chardev_open(console_spec);
    uam_host *host = uam_host_create();
    if (!console || !host) {
        fprintf(stderr, "uam-run: cannot open console '%s'\n", console_spec);
        goto out_host;
    }
    cfg.console = console;
    /* No console input reaches the guest until the run starts: the board's
     * power-on reset, or a snapshot restore, would otherwise discard fixed
     * input (--console string:..., file:...,in=...) handed over while the
     * board attaches its UART. */
    uam_chardev_hold(console, true);
    bool host_net = false;   /* a port is wired to a host interface */
    for (size_t i = 0; i < cfg.n_netdevs; i++) {
        nics[i].nd = uam_netdev_open(nic_specs[i]);
        if (!nics[i].nd) {
            fprintf(stderr, "uam-run: cannot open network backend '%s' for port '%s'\n",
                    nic_specs[i], nics[i].name);
            goto out_host;
        }
        uam_host_add_netdev(host, nics[i].nd);
        host_net |= uam_netdev_fd(nics[i].nd) >= 0;
    }

    uam_machine m;
    uam_status st = board->build(&m, &cfg);
    if (st != UAM_OK) {
        fprintf(stderr, "uam-run: building board '%s' failed: %s\n", board_name, uam_status_str(st));
        uam_machine_fini(&m);
        goto out_host;
    }
    for (size_t i = 0; i < cfg.n_netdevs; i++)
        if (!nics[i].nd->receive)
            fprintf(stderr, "uam-run: board '%s' has no network port '%s'; --nic %s=... is unused\n",
                    board_name, nics[i].name, nics[i].name);
    if (mmio_input_path || mmio_input_rule) {
        if (mmio_input_path && !read_file(mmio_input_path, &mmio_input_data, &mmio_input_len)) {
            fprintf(stderr, "uam-run: cannot read --mmio-input %s\n", mmio_input_path);
            uam_machine_fini(&m);
            goto out_host;
        }
        if (!mmio_input_path && !fuzzing)
            fprintf(stderr, "uam-run: 'input' rules without --mmio-input read 0\n");
        else if (!mmio_input_rule)
            fprintf(stderr, "uam-run: --mmio-input is unused: no --mmio-rule has policy 'input'\n");
        mmio_input = uam_mmio_input_create();
        if (!mmio_input) {
            uam_machine_fini(&m);
            goto out_host;
        }
        uam_mmio_input_set(mmio_input, mmio_input_data, mmio_input_len);
        if (!fuzzing)   /* fuzz inputs run out all the time */
            uam_mmio_input_on_exhausted(mmio_input, on_mmio_input_exhausted, mmio_input);
    }
    if (n_mmio_rules || mmio_input) {
        st = uam_board_add_mmio_rules(&m, mmio_rules, n_mmio_rules, mmio_input);
        if (st != UAM_OK) {
            fprintf(stderr, "uam-run: --mmio-rule: %s\n", uam_status_str(st));
            uam_machine_fini(&m);
            goto out_host;
        }
    }
    if (show_map) {
        fprintf(stderr, "bus map of '%s':\n", board_name);
        uam_bus_dump_map(&m.bus, stderr);
    }

    uam_image_info info = { 0 };
    if (image) {
        st = uam_load_image(&m.bus, &m.cpu, image, have_raw_base ? raw_base : m.reset_pc, &info);
        if (st != UAM_OK) {
            fprintf(stderr, "uam-run: loading %s failed: %s\n", image, uam_status_str(st));
            uam_machine_fini(&m);
            goto out_host;
        }
        uam_machine_reset(&m, have_entry ? entry : info.entry);
    } else if (have_entry) {
        uam_machine_reset(&m, entry);
    }
    if (load_snap) {
        uam_snapshot *snap;
        st = uam_snapshot_load(load_snap, &snap);
        if (st == UAM_OK) {
            uam_snapshot_info si;
            uam_snapshot_get_info(snap, &si);
            st = uam_snapshot_restore(&m, snap);
            if (st == UAM_OK)
                fprintf(stderr, "uam-run: resumed from %s at pc 0x%llx (icount %llu)\n", load_snap,
                        (unsigned long long)si.pc, (unsigned long long)si.icount);
            uam_snapshot_free(snap);
        }
        if (st != UAM_OK) {
            fprintf(stderr, "uam-run: cannot resume from snapshot %s\n", load_snap);
            uam_machine_fini(&m);
            goto out_host;
        }
    }

    m.exec.trace = trace;
    m.exec.trace_lo = trace_lo;
    m.exec.trace_hi = trace_hi;
    /* --trace-after compares with the absolute instruction count, which a snapshot
     * restores; +N is relative to where this run starts. */
    uint64_t start_icount = uam_machine_cpu(&m, 0)->icount;
    if (trace_after_rel)
        trace_after += start_icount;
    else if (load_snap && (trace & UAM_TRACE_BLOCKS) && trace_after && trace_after <= start_icount)
        fprintf(stderr, "uam-run: --trace-after %llu is before the snapshot (icount %llu): blocks are "
                "traced from the start; --trace-after +N counts from the snapshot\n",
                (unsigned long long)trace_after, (unsigned long long)start_icount);
    m.exec.trace_after = trace_after;
    m.exec.verify = verify;
    m.exec.optimize = optimize;
    m.exec.chain = chain;
    if (use_jit && !uam_exec_enable_jit(&m.exec))
        fprintf(stderr, "uam-run: the JIT is unavailable on this host; using the interpreter\n");
    if (block_insns)
        m.exec.max_block_insns = block_insns;
    log_hits = calloc(log_pcs.n ? log_pcs.n : 1, sizeof(*log_hits));
    bool hooks_ok = log_hits != NULL;
    for (unsigned i = 0; hooks_ok && i < log_pcs.n; i++)
        hooks_ok = uam_machine_add_pc_hook(&m, log_pcs.pc[i], log_pc_hook, &log_hits[i]) == UAM_OK;
    snap_at snap_hook = { .m = &m, .path = save_snap };
    if (hooks_ok && have_snap_at)
        hooks_ok = uam_machine_add_pc_hook(&m, snap_at_pc, snapshot_at_hook, &snap_hook) == UAM_OK;
    for (unsigned i = 0; hooks_ok && i < break_pcs.n; i++)
        hooks_ok = uam_machine_add_pc_hook(&m, break_pcs.pc[i], break_hook, NULL) == UAM_OK;
    if (!hooks_ok) {
        fprintf(stderr, "uam-run: cannot install the --log-pc/--break/--snapshot-at hooks\n");
        uam_machine_fini(&m);
        goto out_host;
    }
    uam_runner_setup xs = { .m = &m, .have_image = image != NULL, .image = info,
                            .have_raw_base = have_raw_base, .raw_base = raw_base,
                            .map_size_option = cov_map_size_given ? (uint32_t)cov_map_size : 0 };
    for (size_t e = 0; e < es->n; e++) {
        if (!es->ext[e]->setup)
            continue;
        uam_runner_setup one = xs;
        char err[512] = "";
        if (es->ext[e]->setup(es->st[e], &one, err, sizeof(err)) != UAM_OK) {
            fprintf(stderr, "uam-run: %s: %s\n", es->ext[e]->name, err);
            uam_machine_fini(&m);
            goto out_host;
        }
        if (one.sites) {
            xs.sites = one.sites; xs.n_sites = one.n_sites; xs.scheme = one.scheme;
            xs.history = one.history; xs.skip_repeats = one.skip_repeats;
            xs.map_size = one.map_size; xs.namer = one.namer; xs.namer_ctx = one.namer_ctx;
        }
        if (one.have_begin) {
            xs.have_begin = true; xs.begin_pc = one.begin_pc; xs.end_pcs = one.end_pcs;
            xs.n_end_pcs = one.n_end_pcs; xs.nested = one.nested;
            xs.more_begin_pcs = one.more_begin_pcs; xs.n_more_begin_pcs = one.n_more_begin_pcs;
        }
        if (one.frontend) {
            xs.frontend = one.frontend;
            xs.frontend_ctx = one.frontend_ctx;
        }
        if (one.n_replay_sidecars) {
            xs.replay_sidecars = one.replay_sidecars;
            xs.n_replay_sidecars = one.n_replay_sidecars;
        }
    }
    if (xs.map_size)
        cov_map_size = xs.map_size;
    if (xs.sites) {
        fz.history = xs.history;
        fz.skip_repeats = xs.skip_repeats;
    }
    if (xs.have_begin) {
        fz.begin_pc = xs.begin_pc;
        fz.more_begin_pcs = xs.more_begin_pcs;
        fz.n_more_begin_pcs = xs.n_more_begin_pcs;
        fz.end_pcs = xs.end_pcs;
        fz.n_end_pcs = xs.n_end_pcs;
        fz.nested = xs.nested;
    }
    if (xs.sites && !fuzzing) {
        char err[256];
        cov_map = calloc(1, cov_map_size);
        uam_cov_state_init(&cov_state, cov_map, (uint32_t)cov_map_size, &xs.history);
        cov_state.skip_repeat = xs.skip_repeats;
        if (!cov_map || uam_machine_set_coverage(&m, xs.sites, xs.n_sites, &cov_state, err,
                                                 sizeof(err)) != UAM_OK) {
            fprintf(stderr, "uam-run: cannot enable coverage: %s\n", err);
            uam_machine_fini(&m);
            goto out_host;
        }
        for (unsigned i = 0; xs.namer && i < uam_machine_ncpus(&m); i++)
            uam_cov_pass_set_namer(uam_machine_exec(&m, i)->cov_pass, xs.namer, xs.namer_ctx);
    }
    if (cov_blocks && !fuzzing) {
        cov_map = calloc(1, cov_map_size);
        uam_cov_state_init(&cov_state, cov_map, (uint32_t)cov_map_size, NULL);
        if (!cov_map || uam_machine_set_block_coverage(&m, &cov_state) != UAM_OK) {
            fprintf(stderr, "uam-run: cannot enable coverage\n");
            uam_machine_fini(&m);
            goto out_host;
        }
    }

    uam_host_add_chardev(host, console);
    /* Guest network stacks time out in virtual time: with virtual pacing an idle
     * guest's retransmit/ARP timers would expire before any host reply arrived. */
    if (host_net && !realtime && !virtual_time) {
        uam_log(UAM_LOG_INFO, "uam-run", "host network attached: pacing virtual time to the host "
                "clock (--virtual-time to keep it deterministic)");
        realtime = true;
    }
    uam_host_set_pacing(host, realtime ? UAM_PACE_REALTIME : UAM_PACE_VIRTUAL, realtime_tps, 0);
    m.exec.max_stall = max_stall;
    if (quantum)
        uam_machine_set_quantum(&m, quantum);
    if (max_ticks) {
        uint64_t now = uam_sched_now(&m.sched);
        m.exec.max_time = max_ticks < UAM_SCHED_TIME_MAX - now ? now + max_ticks : UAM_SCHED_TIME_MAX;
    }
    uam_host_attach(host, &m.exec);
    uam_chardev_set_quit_handler(console, on_console_quit, &m);

    /* SIGINT/SIGTERM stop the guest at the next instruction boundary, so the
     * normal shutdown (device destroy hooks, statistics) always runs. No
     * SA_RESTART: a host poll() returns early. */
    g_machine = &m;
    struct sigaction sa, old_int, old_term;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, &old_int);
    sigaction(SIGTERM, &sa, &old_term);

    uam_stop_reason why = UAM_STOP_NONE;
    uam_chardev_hold(console, false);   /* the machine is set up: input may flow */
    struct timespec run_t0, run_t1;
    const uint64_t run_icount0 = uam_machine_icount(&m);
    clock_gettime(CLOCK_MONOTONIC, &run_t0);
    if (fuzzing) {
        rc = run_fuzz(&m, &fz, cov_blocks, fuzz_input, mmio_input, fuzz_replay, cov_out,
                      cov_map_size, stats, &xs, es);
    } else if (gdb_port) {
        uam_gdb *g = uam_gdb_create(&m);
        if (!g || uam_gdb_listen(g, gdb_port) != UAM_OK) {
            fprintf(stderr, "uam-run: cannot start the gdb server on %s\n", gdb_port);
            uam_gdb_destroy(g);
            why = UAM_STOP_ERROR;
        } else {
            why = uam_gdb_serve(g, max_insns);
            uam_gdb_destroy(g);
        }
    } else {
        why = uam_exec_run(&m.exec, max_insns);
    }
    clock_gettime(CLOCK_MONOTONIC, &run_t1);
    fflush(stdout);

    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGTERM, &old_term, NULL);
    g_machine = NULL;

    switch (fuzzing ? UAM_STOP_NONE : why) {
    case UAM_STOP_NONE:
        break;   /* fuzzing: rc is the harness's */
    case UAM_STOP_REQUEST:
    case UAM_STOP_HALT:
        /* Pass the firmware's status through when it fits the 1..123 range. */
        if (m.exec.stop_status == 0)
            rc = 0;
        else if (m.exec.stop_status == 130)
            rc = 130;
        else if (m.exec.stop_status > 0 && m.exec.stop_status < 124)
            rc = (int)m.exec.stop_status;
        else
            rc = 1;
        break;
    case UAM_STOP_INSN_LIMIT:
    case UAM_STOP_TIME_LIMIT: rc = 124; break;
    case UAM_STOP_DEADLOCK:   rc = 125; break;
    case UAM_STOP_LIVELOCK:   rc = 128; break;
    case UAM_STOP_DEBUG:
    case UAM_STOP_HOOK:       rc = 127; break;
    default:                  rc = 126; break;
    }

    if (!fuzzing)
        fprintf(stderr, "uam-run: %s (status %lld) at pc=0x%llx after %llu instructions\n",
                uam_stop_reason_str(why), (long long)m.exec.stop_status,
                (unsigned long long)m.cpu.pc, (unsigned long long)uam_machine_icount(&m));
    if (stats) {
        uam_exec_stats sum;
        uam_machine_get_stats(&m, &sum);
        const uam_exec_stats *s = &sum;
        fprintf(stderr,
                "stats: blocks translated=%llu executed=%llu exceptions=%llu interrupts=%llu "
                "resets=%llu idle ticks=%llu virtual time=%llu\n",
                (unsigned long long)s->blocks_translated, (unsigned long long)s->blocks_executed,
                (unsigned long long)s->exceptions, (unsigned long long)s->interrupts,
                (unsigned long long)s->resets, (unsigned long long)s->idle_ticks,
                (unsigned long long)uam_sched_now(&m.sched));
        fprintf(stderr, "stats: blocks native=%llu (chained=%llu) interp=%llu  compiled=%llu "
                "interp-marked=%llu\n",
                (unsigned long long)s->blocks_native, (unsigned long long)s->blocks_chained,
                (unsigned long long)(s->blocks_executed - s->blocks_native),
                (unsigned long long)s->blocks_compiled, (unsigned long long)s->blocks_interp);
        const double secs = (double)(run_t1.tv_sec - run_t0.tv_sec)
                          + (double)(run_t1.tv_nsec - run_t0.tv_nsec) / 1e9;
        const uint64_t ran = uam_machine_icount(&m) - run_icount0;
        fprintf(stderr, "stats: time=%.3fs instructions=%llu MIPS=%.1f translate=%.3fs "
                "ir ops lifted=%llu final=%llu code bytes=%llu\n",
                secs, (unsigned long long)ran, secs > 0 ? (double)ran / secs / 1e6 : 0.0,
                (double)s->translate_ns / 1e9, (unsigned long long)s->ir_ops_lifted,
                (unsigned long long)s->ir_ops_final, (unsigned long long)s->code_bytes);
        if (s->blocks_compiled)
            fprintf(stderr, "stats: jit calls eval=%llu load=%llu store=%llu\n",
                    (unsigned long long)s->jit_eval_calls, (unsigned long long)s->jit_load_calls,
                    (unsigned long long)s->jit_store_calls);
        if (mmio_input)
            fprintf(stderr, "stats: mmio input used=%zu of %zu bytes\n",
                    uam_mmio_input_pos(mmio_input), mmio_input_len);
        if (cov_map) {
            uint64_t hit = 0;
            for (uint64_t i = 0; i < cov_map_size; i++)
                hit += cov_map[i] != 0;
            fprintf(stderr, "stats: coverage map=%llu bytes, buckets hit=%llu\n",
                    (unsigned long long)cov_map_size, (unsigned long long)hit);
        }
    }
    if (cov_out && !fuzzing) {
        FILE *f = fopen(cov_out, "wb");
        bool ok = f && fwrite(cov_map, 1, cov_map_size, f) == cov_map_size;
        if (f && fclose(f) != 0)
            ok = false;
        if (!ok) {
            fprintf(stderr, "uam-run: cannot write the coverage map to %s\n", cov_out);
            if (rc == 0)
                rc = 2;
        }
    }
    if (dump) {
        for (unsigned i = 0, n = uam_machine_ncpus(&m); i < n; i++) {
            if (n > 1)
                fprintf(stderr, "cpu %u: icount=%llu%s\n", i,
                        (unsigned long long)uam_machine_cpu(&m, i)->icount,
                        uam_machine_cpu(&m, i)->waiting ? " waiting" : "");
            m.cpu.isa->dump_state(uam_machine_cpu(&m, i), stderr);
        }
    }
    for (unsigned i = 0; i < n_mem_dumps && !fuzzing; i++)
        if (!dump_memory(&m, &mem_dumps[i]) && rc == 0)
            rc = 2;
    if (save_snap && !have_snap_at) {
        if (why == UAM_STOP_ERROR || why == UAM_STOP_FAULT_LOOP)
            fprintf(stderr, "uam-run: not saving a snapshot after an internal error\n");
        else if (uam_machine_save_snapshot(&m, save_snap) == UAM_OK)
            fprintf(stderr, "uam-run: snapshot saved to %s\n", save_snap);
        else {
            fprintf(stderr, "uam-run: saving a snapshot to %s failed\n", save_snap);
            if (rc == 0)
                rc = 2;
        }
    }
    if (have_snap_at && !snap_hook.done)
        fprintf(stderr, "uam-run: --snapshot-at 0x%llx was never reached; no snapshot saved\n",
                (unsigned long long)snap_at_pc);

    for (size_t e = 0; e < es->n; e++)
        if (es->ext[e]->finish)
            rc = es->ext[e]->finish(es->st[e], &m, fuzzing, rc);

    uam_host_destroy(host);  /* detach from the executor before it goes away */
    host = NULL;
    uam_machine_fini(&m);
out_host:
    uam_host_destroy(host);
    for (size_t i = 0; i < cfg.n_netdevs; i++)
        uam_netdev_destroy(nics[i].nd);   /* after the machine: devices detach first */
    uam_chardev_destroy(console);
    uam_mmio_input_free(mmio_input);   /* after the machine: its stubs read it */
    free(mmio_input_data);
    free(cov_map);   /* after the machine: the hart pointed at it */
    free(uart_input_spec);
    free(log_pcs.pc);
    free(break_pcs.pc);
    free(log_hits);   /* after the machine: the log hooks counted into it */
    return rc;
}

int uam_run_main_ext(int argc, char **argv, const uam_runner_ext *const *exts, size_t n_exts)
{
    enum { MAX_EXTS = 16 };
    void *st[MAX_EXTS] = { 0 };
    if (n_exts > MAX_EXTS) {
        fprintf(stderr, "uam-run: at most %d runner extensions\n", MAX_EXTS);
        return 2;
    }
    for (size_t i = 0; i < n_exts; i++) {
        if (exts[i]->create && !(st[i] = exts[i]->create())) {
            fprintf(stderr, "uam-run: cannot set up extension %s\n", exts[i]->name);
            while (i--)
                if (exts[i]->destroy)
                    exts[i]->destroy(st[i]);
            return 2;
        }
    }
    g_exts = exts;
    g_n_exts = n_exts;
    ext_set es = { .ext = exts, .st = st, .n = n_exts };
    int rc = run_main(argc, argv, &es);
    for (size_t i = 0; i < n_exts; i++)
        if (exts[i]->destroy)
            exts[i]->destroy(st[i]);
    g_exts = NULL;
    g_n_exts = 0;
    return rc;
}

int uam_run_main(int argc, char **argv)
{
    return uam_run_main_ext(argc, argv, uam_runner_stock_exts, uam_runner_n_stock_exts);
}
