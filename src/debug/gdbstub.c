/*
 * gdbstub.c - gdb remote serial protocol server (uam/gdbstub.h).
 *
 * One client. Every processor of the machine is a thread (id = processor
 * number + 1) in all-stop mode: the whole machine runs or stops together, a
 * stop reply names the thread that caused it, and registers, memory
 * translation and single steps act on the thread gdb selected (Hg).
 * Packets supported: ? g G p P m M c s vCont vCont? Z0-Z4 z0-z4 k D
 * qSupported qXfer:features:read qAttached qC qfThreadInfo qsThreadInfo
 * qThreadExtraInfo H T. Anything else gets the empty reply, which gdb treats
 * as "not supported".
 */
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "uam/gdbstub.h"

#define PKT_MAX   0x4000u
#define MAX_BP    64u
#define SLICE     100000u   /* instructions between checks for Ctrl-C */

typedef struct gdb_bp {
    uint64_t addr;
    bool     used;
} gdb_bp;

typedef struct gdb_wp {
    uint64_t vaddr, paddr, len;
    int      type;     /* 2 write, 3 read, 4 access */
    int      id;
    bool     used;
} gdb_wp;

struct uam_gdb {
    uam_machine *m;
    int          listen_fd, fd;
    uint8_t      rbuf[512];
    size_t       rpos, rlen;
    bool         big;            /* target byte order for registers */
    gdb_bp       bp[MAX_BP];
    gdb_wp       wp[MAX_BP];
    bool         watch_hit;
    uint64_t     watch_addr;
    int          watch_type;
    bool         bp_hit;
    bool         interrupted;
    uint64_t     end_icount;     /* overall instruction limit, all processors (UINT64_MAX: none) */
    unsigned     cur;            /* the selected thread's processor (Hg; a stop selects its own) */
    unsigned     stop_cpu;       /* the processor that hit a breakpoint or watchpoint */
    char         pkt[PKT_MAX];
    char         out[PKT_MAX];
};

/* ---- transport ---------------------------------------------------------- */

static int rd_byte(uam_gdb *g)
{
    if (g->rpos == g->rlen) {
        ssize_t n;
        do {
            n = recv(g->fd, g->rbuf, sizeof(g->rbuf), 0);
        } while (n < 0 && errno == EINTR);
        if (n <= 0)
            return -1;
        g->rpos = 0;
        g->rlen = (size_t)n;
    }
    return g->rbuf[g->rpos++];
}

static void wr_all(uam_gdb *g, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        ssize_t n = send(g->fd, p, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return;
        p += n;
        len -= (size_t)n;
    }
}

static const char hexd[] = "0123456789abcdef";

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void put_packet(uam_gdb *g, const char *data)
{
    size_t len = strlen(data);
    uint8_t cs = 0;
    for (size_t i = 0; i < len; i++)
        cs += (uint8_t)data[i];
    char tail[3] = { '#', hexd[cs >> 4], hexd[cs & 15] };
    for (int tries = 0; tries < 3; tries++) {
        wr_all(g, "$", 1);
        wr_all(g, data, len);
        wr_all(g, tail, 3);
        int c = rd_byte(g);
        if (c != '-')
            return;  /* '+', or the connection went away */
    }
}

/* Read one packet into g->pkt. Returns its length, or -1 on disconnect. */
static int get_packet(uam_gdb *g)
{
    for (;;) {
        int c;
        do {
            c = rd_byte(g);
            if (c < 0)
                return -1;
        } while (c != '$');
        size_t n = 0;
        uint8_t cs = 0;
        while ((c = rd_byte(g)) >= 0 && c != '#') {
            if (n + 1 < sizeof(g->pkt))
                g->pkt[n++] = (char)c;
            cs += (uint8_t)c;
        }
        if (c < 0)
            return -1;
        int h = rd_byte(g), l = rd_byte(g);
        if (h < 0 || l < 0)
            return -1;
        g->pkt[n] = 0;
        if (((hexval(h) << 4) | hexval(l)) != cs) {
            wr_all(g, "-", 1);
            continue;
        }
        wr_all(g, "+", 1);
        return (int)n;
    }
}

/* A Ctrl-C (0x03) pending from the client while the guest runs? */
static bool ctrl_c_pending(uam_gdb *g)
{
    if (g->rpos < g->rlen) {
        if (g->rbuf[g->rpos] == 0x03) {
            g->rpos++;
            return true;
        }
        return false;
    }
    struct pollfd p = { .fd = g->fd, .events = POLLIN };
    if (poll(&p, 1, 0) <= 0)
        return false;
    int c = rd_byte(g);
    return c == 0x03 || c < 0;
}

/* ---- registers and memory ------------------------------------------------- */

static char *put_reg(uam_gdb *g, char *p, uint64_t v, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; i++) {
        unsigned sh = 8 * (g->big ? bytes - 1 - i : i);
        uint8_t b = (uint8_t)(v >> sh);
        *p++ = hexd[b >> 4];
        *p++ = hexd[b & 15];
    }
    return p;
}

static bool get_reg(uam_gdb *g, const char **pp, unsigned bytes, uint64_t *v)
{
    const char *p = *pp;
    uint64_t r = 0;
    for (unsigned i = 0; i < bytes; i++) {
        int h = hexval(p[0]), l = hexval(p[1]);
        if (h < 0 || l < 0)
            return false;
        unsigned sh = 8 * (g->big ? bytes - 1 - i : i);
        r |= (uint64_t)((h << 4) | l) << sh;
        p += 2;
    }
    *pp = p;
    *v = r;
    return true;
}

static uam_cpu *cur_cpu(uam_gdb *g) { return uam_machine_cpu(g->m, g->cur); }

#define WIDE_MAX 16u   /* bytes in the widest register a description may have */

static unsigned reg_size(const uam_gdb_arch *a, unsigned r)
{
    return a->reg_sizes ? a->reg_sizes[r] : a->reg_bytes;
}

/* Register r as hex at p; NULL if the ISA cannot read it. */
static char *read_one(uam_gdb *g, uam_cpu *cpu, char *p, unsigned r)
{
    const uam_gdb_arch *a = cpu->isa->gdb;
    unsigned n = reg_size(a, r);
    if (n > 8) {
        uint8_t buf[WIDE_MAX];
        if (n > WIDE_MAX || !a->read_wide || !a->read_wide(cpu, r, buf))
            return NULL;
        for (unsigned i = 0; i < n; i++) {
            *p++ = hexd[buf[i] >> 4];
            *p++ = hexd[buf[i] & 15];
        }
        return p;
    }
    uint64_t v = 0;
    if (!a->read_reg(cpu, r, &v))
        return NULL;
    return put_reg(g, p, v, n);
}

/* Register r from the hex at *pp (advanced); false if malformed or not writable. */
static bool write_one(uam_gdb *g, uam_cpu *cpu, const char **pp, unsigned r)
{
    const uam_gdb_arch *a = cpu->isa->gdb;
    unsigned n = reg_size(a, r);
    if (n > 8) {
        uint8_t buf[WIDE_MAX];
        if (n > WIDE_MAX)
            return false;
        for (unsigned i = 0; i < n; i++) {
            int h = hexval((*pp)[0]), l = hexval((*pp)[1]);
            if (h < 0 || l < 0)
                return false;
            buf[i] = (uint8_t)((h << 4) | l);
            *pp += 2;
        }
        return a->write_wide && a->write_wide(cpu, r, buf);
    }
    uint64_t v;
    return get_reg(g, pp, n, &v) && a->write_reg(cpu, r, v);
}

static bool vtop(uam_gdb *g, uint64_t va, uint64_t *pa)
{
    uam_cpu *cpu = cur_cpu(g);
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

static void read_mem(uam_gdb *g, uint64_t addr, uint64_t len)
{
    char *p = g->out;
    if (len > (PKT_MAX - 4) / 2)
        len = (PKT_MAX - 4) / 2;
    for (uint64_t i = 0; i < len; i++) {
        uint64_t pa, b;
        if (!vtop(g, addr + i, &pa)
            || uam_bus_read(&g->m->bus, pa, 1, UAM_ACC_LE | UAM_ACC_DEBUG, &b) != UAM_OK) {
            if (i == 0) {
                put_packet(g, "E14");
                return;
            }
            break;
        }
        *p++ = hexd[(b >> 4) & 15];
        *p++ = hexd[b & 15];
    }
    *p = 0;
    put_packet(g, g->out);
}

static void write_mem(uam_gdb *g, uint64_t addr, uint64_t len, const char *hex)
{
    for (uint64_t i = 0; i < len; i++) {
        int h = hexval(hex[2 * i]), l = hexval(hex[2 * i + 1]);
        uint64_t pa;
        if (h < 0 || l < 0 || !vtop(g, addr + i, &pa)
            || uam_bus_write(&g->m->bus, pa, 1, UAM_ACC_LE | UAM_ACC_DEBUG,
                             (uint64_t)((h << 4) | l)) != UAM_OK) {
            put_packet(g, "E14");
            return;
        }
    }
    put_packet(g, "OK");
}

/* ---- breakpoints and watchpoints ------------------------------------------ */

static uam_hook_action bp_hook(void *ctx, uam_cpu *cpu, uint64_t pc)
{
    (void)pc;
    uam_gdb *g = ctx;
    if (!g->bp_hit)
        g->stop_cpu = cpu->hartid;
    g->bp_hit = true;
    return UAM_HOOK_STOP;
}

static void watch_cb(void *ctx, uint64_t addr, unsigned size, bool is_write, uint64_t val)
{
    (void)size; (void)val;
    uam_gdb *g = ctx;
    for (unsigned i = 0; i < MAX_BP; i++) {
        gdb_wp *w = &g->wp[i];
        if (!w->used || addr + size <= w->paddr || addr >= w->paddr + w->len)
            continue;
        if (w->type == 2 && !is_write)
            continue;
        if (w->type == 3 && is_write)
            continue;
        g->watch_hit = true;
        g->watch_type = w->type;
        g->watch_addr = w->vaddr + (addr > w->paddr ? addr - w->paddr : 0);
        /* the processor whose access this is */
        g->stop_cpu = uam_machine_current_cpu(g->m);
        /* Stop after this instruction, without touching the guest's stop status. */
        g->m->exec.stop_request = true;
        uam_cpu_request_exit(uam_machine_cpu(g->m, g->stop_cpu));
        return;
    }
}

static uint64_t state_bits(const uam_gdb *g)
{
    return uam_isa_pc_state_bits(g->m->cpu.isa);
}

/* A breakpoint at addr in whichever instruction-set state the code there runs in. */
static bool bp_hooks(uam_gdb *g, uint64_t addr, bool insert)
{
    const uint64_t sb = state_bits(g);
    if (insert) {   /* on every processor */
        if (uam_machine_add_pc_hook(g->m, addr, bp_hook, g) != UAM_OK)
            return false;
        if (sb && uam_machine_add_pc_hook(g->m, addr | sb, bp_hook, g) != UAM_OK) {
            uam_machine_remove_pc_hook(g->m, addr, bp_hook, g);
            return false;
        }
        return true;
    }
    uam_machine_remove_pc_hook(g->m, addr, bp_hook, g);
    if (sb)
        uam_machine_remove_pc_hook(g->m, addr | sb, bp_hook, g);
    return true;
}

static bool set_point(uam_gdb *g, int type, uint64_t addr, uint64_t len, bool insert)
{
    if (type == 0 || type == 1) {
        addr &= ~state_bits(g);
        for (unsigned i = 0; i < MAX_BP; i++) {
            gdb_bp *b = &g->bp[i];
            if (insert && !b->used) {
                if (!bp_hooks(g, addr, true))
                    return false;
                *b = (gdb_bp){ .addr = addr, .used = true };
                return true;
            }
            if (!insert && b->used && b->addr == addr) {
                bp_hooks(g, addr, false);
                b->used = false;
                return true;
            }
        }
        return !insert;
    }
    if (type < 2 || type > 4)
        return false;
    for (unsigned i = 0; i < MAX_BP; i++) {
        gdb_wp *w = &g->wp[i];
        if (insert && !w->used) {
            uint64_t pa;
            if (!vtop(g, addr, &pa))
                return false;
            unsigned kinds = type == 2 ? UAM_WATCH_WRITE : type == 3 ? UAM_WATCH_READ
                                                                     : UAM_WATCH_READ | UAM_WATCH_WRITE;
            int id = uam_bus_add_watch(&g->m->bus, pa, len ? len : 1, kinds, watch_cb, g);
            if (id < 0)
                return false;
            *w = (gdb_wp){ .vaddr = addr, .paddr = pa, .len = len ? len : 1, .type = type,
                           .id = id, .used = true };
            return true;
        }
        if (!insert && w->used && w->vaddr == addr && w->type == type) {
            uam_bus_remove_watch(&g->m->bus, w->id);
            w->used = false;
            return true;
        }
    }
    return !insert;
}

static void clear_points(uam_gdb *g)
{
    for (unsigned i = 0; i < MAX_BP; i++) {
        if (g->bp[i].used)
            set_point(g, 0, g->bp[i].addr, 0, false);
        if (g->wp[i].used)
            set_point(g, g->wp[i].type, g->wp[i].vaddr, 0, false);
    }
}

/* ---- execution -------------------------------------------------------------- */

/* Instructions left before the overall limit (0 = unlimited for exec_run). */
static uint64_t remaining(const uam_gdb *g)
{
    return g->end_icount == UINT64_MAX ? 0 : g->end_icount - uam_machine_icount(g->m);
}

/* Run (or step) until something gdb should hear about. A step runs the machine
 * until the selected processor has retired one instruction (the others run on in
 * their turns, as all-stop mode has it). */
static uam_stop_reason resume(uam_gdb *g, bool step)
{
    uam_exec *x = &g->m->exec;
    const uam_cpu *cpu = cur_cpu(g);
    const uint64_t step_from = cpu->icount;
    unsigned polls = 0;
    g->bp_hit = g->watch_hit = g->interrupted = false;
    g->stop_cpu = g->cur;
    for (;;) {
        uint64_t done = uam_machine_icount(g->m);
        if (done >= g->end_icount)
            return UAM_STOP_INSN_LIMIT;
        uint64_t slice = step ? 1 : SLICE, left = g->end_icount - done;
        if (slice > left)
            slice = left;
        uam_stop_reason why = uam_exec_run(x, slice);
        if (why == UAM_STOP_REQUEST && g->watch_hit) {
            x->stop_request = false;
            return UAM_STOP_DEBUG;
        }
        if (why == UAM_STOP_INSN_LIMIT && uam_machine_icount(g->m) < g->end_icount) {
            if (step && cpu->icount != step_from)
                return why;
            if ((!step || ++polls % 4096 == 0) && ctrl_c_pending(g)) {
                g->interrupted = true;
                return UAM_STOP_DEBUG;
            }
            continue;
        }
        return why;
    }
}

/* The stop reply's "thread:" field, with a trailing ';'. */
static const char *thread_field(uam_gdb *g, char *buf, size_t n)
{
    snprintf(buf, n, "thread:%x;", g->stop_cpu + 1);
    return buf;
}

static void report(uam_gdb *g, uam_stop_reason why, bool *done)
{
    *done = false;
    switch (why) {
    case UAM_STOP_REQUEST:
    case UAM_STOP_HALT: {
        char buf[8];
        snprintf(buf, sizeof(buf), "W%02x", (unsigned)(g->m->exec.stop_status & 0xFF));
        put_packet(g, buf);
        *done = true;
        return;
    }
    case UAM_STOP_ERROR:
    case UAM_STOP_FAULT_LOOP:
        put_packet(g, "T06");  /* SIGABRT: inspect the state */
        return;
    case UAM_STOP_TIME_LIMIT:
        put_packet(g, "T0e");      /* SIGALRM: the virtual time budget is spent */
        return;
    case UAM_STOP_INSN_LIMIT:
        if (uam_machine_icount(g->m) >= g->end_icount) {
            put_packet(g, "T0e");  /* SIGALRM: the instruction budget is spent */
            return;
        }
        break;
    default:
        break;
    }
    /* gdb switches to the thread a stop names; so do we */
    g->cur = g->stop_cpu;
    char th[32], buf[96];
    thread_field(g, th, sizeof(th));
    if (g->interrupted) {
        snprintf(buf, sizeof(buf), "T02%s", th);
    } else if (g->watch_hit) {
        static const char *const kind[] = { "", "", "watch", "rwatch", "awatch" };
        snprintf(buf, sizeof(buf), "T05%s:%llx;%s", kind[g->watch_type],
                 (unsigned long long)g->watch_addr, th);
    } else if (g->bp_hit) {
        snprintf(buf, sizeof(buf), "T05swbreak:;%s", th);
    } else {
        snprintf(buf, sizeof(buf), "T05%s", th);
    }
    put_packet(g, buf);
}

/* A thread id from a packet: the processor, or -1 for "any"/"all" (0, -1), or
 * UINT32_MAX if there is no such thread. */
static long thread_of(uam_gdb *g, const char *s)
{
    if (!strcmp(s, "-1") || !strcmp(s, "0"))
        return -1;
    char *end;
    unsigned long t = strtoul(s, &end, 16);
    if (*end || t == 0 || t > uam_machine_ncpus(g->m))
        return (long)UINT32_MAX;
    return (long)(t - 1);
}

static void thread_list(uam_gdb *g)
{
    char *o = g->out;
    *o++ = 'm';
    for (unsigned i = 0, n = uam_machine_ncpus(g->m); i < n; i++)
        o += snprintf(o, 16, i ? ",%x" : "%x", i + 1);
    *o = 0;
    put_packet(g, g->out);
}

/* qThreadExtraInfo: "cpu N", and whether it waits. */
static void thread_info(uam_gdb *g, const char *id)
{
    long t = thread_of(g, id);
    if (t < 0 || t == (long)UINT32_MAX) {
        put_packet(g, "E01");
        return;
    }
    const uam_cpu *cpu = uam_machine_cpu(g->m, (unsigned)t);
    char text[48];
    snprintf(text, sizeof(text), "cpu %ld%s", t, cpu->waiting ? " (waiting)" : "");
    char *o = g->out;
    for (const char *c = text; *c; c++) {
        *o++ = hexd[(*c >> 4) & 15];
        *o++ = hexd[*c & 15];
    }
    *o = 0;
    put_packet(g, g->out);
}

/* ---- packet dispatch ------------------------------------------------------- */

static void handle_qxfer(uam_gdb *g, const char *args)
{
    const uam_gdb_arch *a = g->m->cpu.isa->gdb;
    const char *prefix = "target.xml:";
    if (strncmp(args, prefix, strlen(prefix)) != 0) {
        put_packet(g, "E00");
        return;
    }
    unsigned long off = 0, len = 0;
    sscanf(args + strlen(prefix), "%lx,%lx", &off, &len);
    size_t total = strlen(a->target_xml);
    if (off >= total) {
        put_packet(g, "l");
        return;
    }
    if (len > PKT_MAX - 2)
        len = PKT_MAX - 2;
    size_t n = total - off < len ? total - off : len;
    g->out[0] = off + n >= total ? 'l' : 'm';
    memcpy(g->out + 1, a->target_xml + off, n);
    g->out[1 + n] = 0;
    put_packet(g, g->out);
}

uam_stop_reason uam_gdb_serve(uam_gdb *g, uint64_t max_insns)
{
    const uam_gdb_arch *a = g->m->cpu.isa->gdb;
    uam_stop_reason last = UAM_STOP_DEBUG;
    g->end_icount = max_insns ? uam_machine_icount(g->m) + max_insns : UINT64_MAX;

    for (;;) {
        uam_cpu *cpu = cur_cpu(g);
        int n = get_packet(g);
        if (n < 0) {
            /* Client vanished: behave like a detach. */
            clear_points(g);
            return uam_exec_run(&g->m->exec, remaining(g));
        }
        char *p = g->pkt;
        switch (p[0]) {
        case '?': {
            char buf[32];
            snprintf(buf, sizeof(buf), "T05thread:%x;", g->cur + 1);
            put_packet(g, buf);
            break;
        }
        case 'g': {
            char *o = g->out;
            for (unsigned r = 0; r < a->n_regs; r++) {
                char *next = read_one(g, cpu, o, r);
                if (!next) {   /* unreadable: zeros */
                    next = o;
                    for (unsigned i = 0; i < 2 * reg_size(a, r); i++)
                        *next++ = '0';
                }
                o = next;
            }
            *o = 0;
            put_packet(g, g->out);
            break;
        }
        case 'G': {
            /* Registers that cannot be written (x86 selectors) keep their value. */
            const char *q = p + 1;
            for (unsigned r = 0; r < a->n_regs && strlen(q) >= 2 * reg_size(a, r); r++) {
                const char *at = q;
                if (!write_one(g, cpu, &q, r))
                    q = at + 2 * reg_size(a, r);
            }
            put_packet(g, "OK");
            break;
        }
        case 'p': {
            unsigned r = (unsigned)strtoul(p + 1, NULL, 16);
            char *o = r < a->n_regs ? read_one(g, cpu, g->out, r) : NULL;
            if (!o) {
                put_packet(g, "E01");
                break;
            }
            *o = 0;
            put_packet(g, g->out);
            break;
        }
        case 'P': {
            char *eq = strchr(p, '=');
            unsigned r = (unsigned)strtoul(p + 1, NULL, 16);
            const char *q = eq ? eq + 1 : "";
            put_packet(g, eq && r < a->n_regs && write_one(g, cpu, &q, r) ? "OK" : "E01");
            break;
        }
        case 'm': {
            unsigned long long addr = 0, len = 0;
            sscanf(p + 1, "%llx,%llx", &addr, &len);
            read_mem(g, addr, len);
            break;
        }
        case 'M': {
            unsigned long long addr = 0, len = 0;
            char *colon = strchr(p, ':');
            sscanf(p + 1, "%llx,%llx", &addr, &len);
            if (!colon || strlen(colon + 1) < 2 * len)
                put_packet(g, "E01");
            else
                write_mem(g, addr, len, colon + 1);
            break;
        }
        case 'c':
        case 's': {
            if (p[1])   /* an address, in the current instruction-set state */
                cpu->pc = (strtoull(p + 1, NULL, 16) & ~state_bits(g)) | (cpu->pc & state_bits(g));
            bool done;
            last = resume(g, p[0] == 's');
            report(g, last, &done);
            if (done)
                return last;
            break;
        }
        case 'v':
            if (!strcmp(p, "vCont?")) {
                put_packet(g, "vCont;c;C;s;S");
            } else if (!strncmp(p, "vCont;", 6)) {
                /* The first action: all-stop, so the machine runs as a whole; a
                 * step steps the thread it names (or the selected one). */
                char act = p[6];
                if (act == 's' || act == 'S') {
                    const char *colon = strchr(p + 6, ':');
                    if (colon) {
                        char id[16];
                        snprintf(id, sizeof(id), "%.*s", (int)strcspn(colon + 1, ";"), colon + 1);
                        long t = thread_of(g, id);
                        if (t >= 0 && t != (long)UINT32_MAX)
                            g->cur = (unsigned)t;
                    }
                }
                bool done;
                last = resume(g, act == 's' || act == 'S');
                report(g, last, &done);
                if (done)
                    return last;
            } else {
                put_packet(g, "");
            }
            break;
        case 'Z':
        case 'z': {
            int type = p[1] - '0';
            unsigned long long addr = 0, len = 0;
            sscanf(p + 3, "%llx,%llx", &addr, &len);
            put_packet(g, set_point(g, type, addr, len, p[0] == 'Z') ? "OK" : "E01");
            break;
        }
        case 'k':
            clear_points(g);
            g->m->exec.stop_status = 130;
            return UAM_STOP_REQUEST;
        case 'D':
            put_packet(g, "OK");
            clear_points(g);
            close(g->fd);
            g->fd = -1;
            return uam_exec_run(&g->m->exec, remaining(g));
        case 'H': {   /* Hg selects the thread for registers and memory; Hc is all-stop */
            long t = thread_of(g, p + 2);
            if (t == (long)UINT32_MAX) {
                put_packet(g, "E01");
                break;
            }
            if (p[1] == 'g' && t >= 0)
                g->cur = (unsigned)t;
            put_packet(g, "OK");
            break;
        }
        case 'T': {
            long t = thread_of(g, p + 1);
            put_packet(g, t >= 0 && t != (long)UINT32_MAX ? "OK" : "E01");
            break;
        }
        case 'q':
            if (!strncmp(p, "qSupported", 10))
                put_packet(g, "PacketSize=3fff;qXfer:features:read+;swbreak+;hwbreak+");
            else if (!strncmp(p, "qXfer:features:read:", 20))
                handle_qxfer(g, p + 20);
            else if (!strcmp(p, "qAttached"))
                put_packet(g, "1");
            else if (!strcmp(p, "qC")) {
                char buf[16];
                snprintf(buf, sizeof(buf), "QC%x", g->cur + 1);
                put_packet(g, buf);
            } else if (!strcmp(p, "qfThreadInfo"))
                thread_list(g);
            else if (!strncmp(p, "qThreadExtraInfo,", 17))
                thread_info(g, p + 17);
            else if (!strcmp(p, "qsThreadInfo"))
                put_packet(g, "l");
            else
                put_packet(g, "");
            break;
        default:
            put_packet(g, "");
            break;
        }
    }
}

/* ---- lifecycle ------------------------------------------------------------- */

uam_gdb *uam_gdb_create(uam_machine *m)
{
    if (!m->cpu.isa->gdb) {
        uam_log(UAM_LOG_ERROR, "gdb", "ISA %s has no debugger description", m->cpu.isa->name);
        return NULL;
    }
    uam_gdb *g = calloc(1, sizeof(*g));
    if (!g)
        return NULL;
    g->m = m;
    g->listen_fd = g->fd = -1;
    g->big = m->cpu.isa->default_endian & UAM_ACC_BE;
    return g;
}

uam_status uam_gdb_listen(uam_gdb *g, const char *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)atoi(port)),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (fd < 0)
        return UAM_ERR_INVALID;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 1) != 0) {
        uam_log(UAM_LOG_ERROR, "gdb", "cannot listen on port %s: %s", port, strerror(errno));
        close(fd);
        return UAM_ERR_INVALID;
    }
    g->listen_fd = fd;
    uam_log(UAM_LOG_WARN, "gdb", "waiting for gdb on localhost:%s", port);
    int c;
    do {
        c = accept(fd, NULL, NULL);
    } while (c < 0 && errno == EINTR && !g->m->exec.stop_request);
    if (c < 0)
        return UAM_ERR_INVALID;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    g->fd = c;
    uam_log(UAM_LOG_WARN, "gdb", "gdb connected");
    return UAM_OK;
}

void uam_gdb_destroy(uam_gdb *g)
{
    if (!g)
        return;
    clear_points(g);
    if (g->fd >= 0)
        close(g->fd);
    if (g->listen_fd >= 0)
        close(g->listen_fd);
    free(g);
}
