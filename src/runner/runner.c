/*
 * runner.c - The stock command-line runner (uam/runner.h).
 *
 * Exit status:
 *   finisher/halt status (0 = pass) when the firmware ends the simulation,
 *   124 instruction limit, 125 deadlock, 126 internal error or fault loop,
 *   127 debug trap or breakpoint, 130 interrupted (SIGINT/SIGTERM),
 *   2 usage or setup error.
 */
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/gdbstub.h"
#include "uam/host.h"
#include "uam/loader.h"
#include "uam/machine.h"
#include "uam/runner.h"

#define MAX_LIST 64

static void usage(FILE *out)
{
    fprintf(out,
        "usage: uam-run [options] [IMAGE]\n"
        "  -b, --board NAME        board to build (default: virt); --board list shows all\n"
        "      --raw-base ADDR     load address for raw (non-ELF) images (default: board reset pc)\n"
        "      --image NAME=PATH   named image for the board to place (repeatable)\n"
        "  -o, --option KEY=VALUE  board-specific option (repeatable)\n"
        "  -n, --max-insns N       stop after N guest instructions (0 = unlimited)\n"
        "      --ram SIZE          RAM size in bytes (suffix K/M/G allowed)\n"
        "      --unknown-mmio P    unmapped accesses: fault (default), zero, echo\n"
        "      --console SPEC      console backend: stdio (default), stdio-raw, null, pty,\n"
        "                          tcp:[HOST:]PORT, unix:PATH, file:OUT[,in=FILE], string:TEXT\n"
        "      --uart-input STR    shorthand for --console string:STR\n"
        "      --realtime[=TPS]    pace virtual time to the host clock (TPS ticks/s, default 100M)\n"
        "      --block-insns N     max guest instructions per block (default 32)\n"
        "  -t, --trace LIST        comma list of: blocks, ir, exc\n"
        "      --trace-range LO:HI trace blocks only for pcs in [LO, HI)\n"
        "      --trace-after N     trace blocks only after N instructions\n"
        "      --log-pc ADDR       log each arrival at ADDR (repeatable)\n"
        "      --break ADDR        stop when ADDR is reached (repeatable)\n"
        "      --gdb PORT          wait for a gdb connection on localhost:PORT\n"
        "      --map               print the bus map after building the board\n"
        "      --no-verify         skip IR verification of translated blocks\n"
        "      --no-opt            do not optimize translated blocks\n"
        "      --jit               compile blocks to native code (x86-64 host)\n"
        "  -s, --stats             print execution statistics at exit\n"
        "  -d, --dump-state        print hart state at exit\n"
        "  -v, --verbose           more logging (repeatable)\n"
        "  -h, --help\n");
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

static void on_signal(int sig)
{
    (void)sig;
    if (g_machine)
        uam_exec_stop(&g_machine->exec, 130);
}

static void on_console_quit(void *ctx)
{
    uam_exec_stop(&((uam_machine *)ctx)->exec, 0);
}

/* ---- PC hooks from the command line --------------------------------------- */

static uam_hook_action log_pc_hook(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    uint64_t *hits = ctx;
    (*hits)++;
    fprintf(stderr, "uam-run: pc 0x%llx reached (hit %llu, icount %llu)\n", (unsigned long long)pc,
            (unsigned long long)*hits, (unsigned long long)cpu->icount);
    return UAM_HOOK_CONTINUE;
}

static uam_hook_action break_hook(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    (void)ctx;
    fprintf(stderr, "uam-run: breakpoint at 0x%llx (icount %llu)\n", (unsigned long long)pc,
            (unsigned long long)cpu->icount);
    return UAM_HOOK_STOP;
}

int uam_run_main(int argc, char **argv)
{
    enum { OPT_RAW_BASE = 256, OPT_RAM, OPT_UNKNOWN, OPT_UART_IN, OPT_NO_VERIFY, OPT_NO_OPT, OPT_JIT, OPT_BLOCK_INSNS,
           OPT_IMAGE, OPT_CONSOLE, OPT_REALTIME, OPT_TRACE_RANGE, OPT_TRACE_AFTER, OPT_LOG_PC,
           OPT_BREAK, OPT_GDB, OPT_MAP };
    static const struct option opts[] = {
        { "board", required_argument, NULL, 'b' },
        { "raw-base", required_argument, NULL, OPT_RAW_BASE },
        { "image", required_argument, NULL, OPT_IMAGE },
        { "option", required_argument, NULL, 'o' },
        { "max-insns", required_argument, NULL, 'n' },
        { "ram", required_argument, NULL, OPT_RAM },
        { "unknown-mmio", required_argument, NULL, OPT_UNKNOWN },
        { "console", required_argument, NULL, OPT_CONSOLE },
        { "uart-input", required_argument, NULL, OPT_UART_IN },
        { "realtime", optional_argument, NULL, OPT_REALTIME },
        { "block-insns", required_argument, NULL, OPT_BLOCK_INSNS },
        { "trace", required_argument, NULL, 't' },
        { "trace-range", required_argument, NULL, OPT_TRACE_RANGE },
        { "trace-after", required_argument, NULL, OPT_TRACE_AFTER },
        { "log-pc", required_argument, NULL, OPT_LOG_PC },
        { "break", required_argument, NULL, OPT_BREAK },
        { "gdb", required_argument, NULL, OPT_GDB },
        { "map", no_argument, NULL, OPT_MAP },
        { "no-verify", no_argument, NULL, OPT_NO_VERIFY },
        { "no-opt", no_argument, NULL, OPT_NO_OPT },
        { "jit", no_argument, NULL, OPT_JIT },
        { "stats", no_argument, NULL, 's' },
        { "dump-state", no_argument, NULL, 'd' },
        { "verbose", no_argument, NULL, 'v' },
        { "help", no_argument, NULL, 'h' },
        { 0 },
    };

    const char *board_name = "virt", *console_spec = "stdio", *gdb_port = NULL;
    char *uart_input_spec = NULL;
    uint64_t raw_base = 0, max_insns = 0, trace_lo = 0, trace_hi = 0, trace_after = 0;
    uint64_t realtime_tps = 0, log_pcs[MAX_LIST], break_pcs[MAX_LIST], log_hits[MAX_LIST] = { 0 };
    unsigned n_log = 0, n_break = 0;
    bool have_raw_base = false, stats = false, dump = false, verify = true, optimize = true, use_jit = false, realtime = false;
    bool show_map = false;
    unsigned trace = 0, block_insns = 0;
    uam_board_option options[MAX_LIST];
    uam_board_image images[MAX_LIST];
    uam_board_config cfg = { .unknown_mmio = UAM_UNKNOWN_FAULT, .options = options, .images = images };

    optind = 1;
    int c;
    while ((c = getopt_long(argc, argv, "b:n:t:o:sdvh", opts, NULL)) != -1) {
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
        case OPT_RAW_BASE: raw_base = strtoull(optarg, NULL, 0); have_raw_base = true; break;
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
        case OPT_BLOCK_INSNS: block_insns = (unsigned)strtoul(optarg, NULL, 0); break;
        case OPT_TRACE_RANGE: {
            char *end;
            trace_lo = strtoull(optarg, &end, 0);
            trace_hi = *end == ':' ? strtoull(end + 1, NULL, 0) : 0;
            break;
        }
        case OPT_TRACE_AFTER: trace_after = strtoull(optarg, NULL, 0); break;
        case OPT_LOG_PC:
            if (n_log < MAX_LIST)
                log_pcs[n_log++] = strtoull(optarg, NULL, 0);
            break;
        case OPT_BREAK:
            if (n_break < MAX_LIST)
                break_pcs[n_break++] = strtoull(optarg, NULL, 0);
            break;
        case OPT_GDB: gdb_port = optarg; break;
        case OPT_MAP: show_map = true; break;
        case OPT_UNKNOWN:
            if (!strcmp(optarg, "fault"))     cfg.unknown_mmio = UAM_UNKNOWN_FAULT;
            else if (!strcmp(optarg, "zero")) cfg.unknown_mmio = UAM_UNKNOWN_ZERO;
            else if (!strcmp(optarg, "echo")) cfg.unknown_mmio = UAM_UNKNOWN_ECHO;
            else { fprintf(stderr, "uam-run: bad --unknown-mmio '%s'\n", optarg); return 2; }
            break;
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

    uam_machine m;
    uam_status st = board->build(&m, &cfg);
    if (st != UAM_OK) {
        fprintf(stderr, "uam-run: building board '%s' failed: %s\n", board_name, uam_status_str(st));
        uam_machine_fini(&m);
        goto out_host;
    }
    if (show_map) {
        fprintf(stderr, "bus map of '%s':\n", board_name);
        uam_bus_dump_map(&m.bus, stderr);
    }

    if (image) {
        uam_image_info info;
        st = uam_load_image(&m.bus, &m.cpu, image, have_raw_base ? raw_base : m.reset_pc, &info);
        if (st != UAM_OK) {
            fprintf(stderr, "uam-run: loading %s failed: %s\n", image, uam_status_str(st));
            uam_machine_fini(&m);
            goto out_host;
        }
        uam_machine_reset(&m, info.entry);
    }

    m.exec.trace = trace;
    m.exec.trace_lo = trace_lo;
    m.exec.trace_hi = trace_hi;
    m.exec.trace_after = trace_after;
    m.exec.verify = verify;
    m.exec.optimize = optimize;
    if (use_jit && !uam_exec_enable_jit(&m.exec))
        fprintf(stderr, "uam-run: the JIT is unavailable on this host; using the interpreter\n");
    if (block_insns)
        m.exec.max_block_insns = block_insns;
    for (unsigned i = 0; i < n_log; i++)
        uam_exec_add_pc_hook(&m.exec, log_pcs[i], log_pc_hook, &log_hits[i]);
    for (unsigned i = 0; i < n_break; i++)
        uam_exec_add_pc_hook(&m.exec, break_pcs[i], break_hook, NULL);

    uam_host_add_chardev(host, console);
    uam_host_set_pacing(host, realtime ? UAM_PACE_REALTIME : UAM_PACE_VIRTUAL, realtime_tps, 0);
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

    uam_stop_reason why;
    if (gdb_port) {
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
    fflush(stdout);

    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGTERM, &old_term, NULL);
    g_machine = NULL;

    switch (why) {
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
    case UAM_STOP_INSN_LIMIT: rc = 124; break;
    case UAM_STOP_DEADLOCK:   rc = 125; break;
    case UAM_STOP_DEBUG:
    case UAM_STOP_HOOK:       rc = 127; break;
    default:                  rc = 126; break;
    }

    fprintf(stderr, "uam-run: %s (status %lld) at pc=0x%llx after %llu instructions\n",
            uam_stop_reason_str(why), (long long)m.exec.stop_status,
            (unsigned long long)m.cpu.pc, (unsigned long long)m.cpu.icount);
    if (stats) {
        const uam_exec_stats *s = &m.exec.stats;
        fprintf(stderr,
                "stats: blocks translated=%llu executed=%llu exceptions=%llu interrupts=%llu "
                "resets=%llu idle ticks=%llu virtual time=%llu\n",
                (unsigned long long)s->blocks_translated, (unsigned long long)s->blocks_executed,
                (unsigned long long)s->exceptions, (unsigned long long)s->interrupts,
                (unsigned long long)s->resets, (unsigned long long)s->idle_ticks,
                (unsigned long long)uam_sched_now(&m.sched));
        fprintf(stderr, "stats: blocks native=%llu interp=%llu  compiled=%llu interp-marked=%llu\n",
                (unsigned long long)s->blocks_native,
                (unsigned long long)(s->blocks_executed - s->blocks_native),
                (unsigned long long)s->blocks_compiled, (unsigned long long)s->blocks_interp);
    }
    if (dump)
        m.cpu.isa->dump_state(&m.cpu, stderr);

    uam_host_destroy(host);  /* detach from the executor before it goes away */
    host = NULL;
    uam_machine_fini(&m);
out_host:
    uam_host_destroy(host);
    uam_chardev_destroy(console);
    free(uart_input_spec);
    return rc;
}
