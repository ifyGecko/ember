/*
 * master_abort_log.c - unclaimed addresses on a PC: reads return all ones, writes are
 * dropped (what firmware expects of an absent TPM, an absent PCI function's MMIO, a
 * disabled decode window), and every first access to an address is logged:
 *
 *   [warn] master-abort: first read of 4 bytes at 0xfed40000 (nothing decodes it: all ones)
 *
 * A silent master abort hides a window the board forgot (a ROM or flash window, a BAR
 * the chipset should have moved): the firmware reads 0xFF.., takes the "absent" path and
 * fails much later. These lines are the to-do list; log_summary.py groups them by page.
 * At exit the hottest 64 KiB pages are printed. Logging state is not guest-visible, so
 * the device is stateless for snapshots (a restored run logs its first accesses again).
 * Local emulation model; not part of ember.
 */
#include <stdlib.h>

#include "uam/common.h"
#include "master_abort_log.h"

#define MAL_ADDRS 4096u    /* distinct addresses remembered (then accesses are only counted) */
#define MAL_PAGES 256u     /* 64 KiB pages in the exit summary */

typedef struct mal_page {
    uint64_t base, reads, writes;
} mal_page;

typedef struct master_abort_log {
    uam_device dev;
    bool       log;
    uint64_t   seen[MAL_ADDRS];   /* open addressing, address + 1 (0 = empty slot) */
    size_t     nseen, unlogged;
    mal_page   pages[MAL_PAGES];
    size_t     npages;
    uint64_t   reads, writes;
} master_abort_log;

static master_abort_log *to_mal(uam_device *d) { return UAM_CONTAINER_OF(d, master_abort_log, dev); }

/* true the first time `addr` is seen (while the table has room) */
static bool first_access(master_abort_log *s, uint64_t addr)
{
    uint64_t key = addr + 1;
    size_t i = (size_t)((addr * 0x9E3779B97F4A7C15ull) >> 52) & (MAL_ADDRS - 1);
    for (;; i = (i + 1) & (MAL_ADDRS - 1)) {
        if (s->seen[i] == key)
            return false;
        if (!s->seen[i])
            break;
    }
    if (s->nseen >= MAL_ADDRS * 3 / 4) {
        s->unlogged++;
        return false;
    }
    s->seen[i] = key;
    s->nseen++;
    return true;
}

static void account(master_abort_log *s, uint64_t addr, bool write)
{
    uint64_t base = addr & ~UINT64_C(0xFFFF);
    write ? s->writes++ : s->reads++;
    for (size_t i = 0; i < s->npages; i++)
        if (s->pages[i].base == base) {
            write ? s->pages[i].writes++ : s->pages[i].reads++;
            return;
        }
    if (s->npages < MAL_PAGES)
        s->pages[s->npages++] = (mal_page){ base, write ? 0 : 1, write ? 1 : 0 };
}

static uam_status mal_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    master_abort_log *s = to_mal(d);
    *val = size >= 8 ? ~0ull : (1ull << (8 * size)) - 1;
    if (s->log) {
        account(s, off, false);
        if (first_access(s, off)) {
            char at[80];
            uam_origin_format(at, sizeof(at));   /* " pc=0x... icount=N": the accessing instruction */
            uam_log(UAM_LOG_WARN, "master-abort", "first read of %u bytes at 0x%llx (nothing decodes it: all ones)%s",
                    size, (unsigned long long)off, at);
        }
    }
    return UAM_OK;
}

static uam_status mal_write(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    master_abort_log *s = to_mal(d);
    if (s->log) {
        account(s, off, true);
        if (first_access(s, off)) {
            char at[80];
            uam_origin_format(at, sizeof(at));
            uam_log(UAM_LOG_WARN, "master-abort", "first write of %u bytes at 0x%llx: 0x%llx (dropped)%s",
                    size, (unsigned long long)off, (unsigned long long)v, at);
        }
    }
    return UAM_OK;
}

static int by_count(const void *a, const void *b)
{
    const mal_page *x = a, *y = b;
    uint64_t cx = x->reads + x->writes, cy = y->reads + y->writes;
    return cx < cy ? 1 : cx > cy ? -1 : (x->base > y->base) - (x->base < y->base);
}

static void mal_destroy(uam_device *d)
{
    master_abort_log *s = to_mal(d);
    if (s->log && s->reads + s->writes) {
        qsort(s->pages, s->npages, sizeof(s->pages[0]), by_count);
        uam_log(UAM_LOG_WARN, "master-abort", "%llu accesses (%llu reads, %llu writes) to %zu addresses%s; hottest 64 KiB pages:",
                (unsigned long long)(s->reads + s->writes), (unsigned long long)s->reads,
                (unsigned long long)s->writes, s->nseen, s->unlogged ? " (and more, not logged)" : "");
        for (size_t i = 0; i < s->npages && i < 8; i++)
            uam_log(UAM_LOG_WARN, "master-abort", "  0x%llx: %llu reads, %llu writes",
                    (unsigned long long)s->pages[i].base, (unsigned long long)s->pages[i].reads,
                    (unsigned long long)s->pages[i].writes);
    }
    free(s);
}

static const uam_device_ops mal_ops = {
    .type = "pc,master-abort-log", .read = mal_read, .write = mal_write, .destroy = mal_destroy,
    .flags = UAM_DEVICE_STATELESS,
};

uam_device *master_abort_log_create(const char *name, bool log)
{
    master_abort_log *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    uam_device_init(&s->dev, &mal_ops, name);
    s->log = log;
    return &s->dev;
}
