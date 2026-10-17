/*
 * tcache.c - Translation cache (uam/exec.h).
 */
#include <stdlib.h>

#include "uam/exec.h"

#define TC_INITIAL_BUCKETS 4096u
#define TC_MAX_BLOCKS      (1u << 18)   /* flush everything beyond this */

struct uam_tc_entry {
    uint64_t             pc;
    uam_block           *blk;
    unsigned             priv;       /* privilege level the block was translated at */
    bool                 mapped;     /* fetched through a non-fixed translation */
    uint32_t             xlate_gen;  /* cpu->xlate_gen at translation (if mapped) */
    struct uam_tc_entry *next;
};

static uint32_t hash_pc(uint64_t pc, uint32_t n)
{
    uint64_t h = (pc >> 1) * UINT64_C(0x9E3779B97F4A7C15);
    return (uint32_t)(h >> 32) & (n - 1);
}

uam_status uam_tcache_init(uam_tcache *tc, uam_bus *bus)
{
    tc->n_buckets = TC_INITIAL_BUCKETS;
    tc->n_blocks = 0;
    tc->bus = bus;
    tc->buckets = calloc(tc->n_buckets, sizeof(*tc->buckets));
    return tc->buckets ? UAM_OK : UAM_ERR_NOMEM;
}

void uam_tcache_flush(uam_tcache *tc)
{
    for (uint32_t i = 0; i < tc->n_buckets; i++) {
        struct uam_tc_entry *e = tc->buckets[i];
        while (e) {
            struct uam_tc_entry *n = e->next;
            uam_block_free(e->blk);
            free(e);
            e = n;
        }
        tc->buckets[i] = NULL;
    }
    tc->n_blocks = 0;
}

void uam_tcache_fini(uam_tcache *tc)
{
    if (tc->buckets)
        uam_tcache_flush(tc);
    free(tc->buckets);
    tc->buckets = NULL;
}

static bool still_valid(uam_tcache *tc, const struct uam_tc_entry *e, unsigned priv,
                        uint32_t xlate_gen)
{
    const uam_block *blk = e->blk;
    if (e->priv != priv || (e->mapped && e->xlate_gen != xlate_gen))
        return false;
    for (uint32_t i = 0; i < blk->n_pages; i++)
        if (uam_bus_page_gen(tc->bus, blk->page[i]) != blk->page_gen[i])
            return false;
    return true;
}

uam_block *uam_tcache_lookup(uam_tcache *tc, uint64_t pc, unsigned priv, uint32_t xlate_gen)
{
    struct uam_tc_entry **pp = &tc->buckets[hash_pc(pc, tc->n_buckets)];
    for (; *pp; pp = &(*pp)->next) {
        struct uam_tc_entry *e = *pp;
        if (e->pc != pc)
            continue;
        if (still_valid(tc, e, priv, xlate_gen))
            return e->blk;
        /* Stale: its code was overwritten, its mapping changed, or it was
         * translated at another privilege level. Drop it. */
        *pp = e->next;
        uam_block_free(e->blk);
        free(e);
        tc->n_blocks--;
        return NULL;
    }
    return NULL;
}

uam_status uam_tcache_insert(uam_tcache *tc, uam_block *blk, unsigned priv, bool mapped,
                             uint32_t xlate_gen)
{
    if (tc->n_blocks >= TC_MAX_BLOCKS)
        uam_tcache_flush(tc);
    struct uam_tc_entry **head = &tc->buckets[hash_pc(blk->guest_pc, tc->n_buckets)];
    for (struct uam_tc_entry *e = *head; e; e = e->next) {
        if (e->pc == blk->guest_pc) {
            uam_block_free(e->blk);
            e->blk = blk;
            e->priv = priv;
            e->mapped = mapped;
            e->xlate_gen = xlate_gen;
            return UAM_OK;
        }
    }
    struct uam_tc_entry *e = malloc(sizeof(*e));
    if (!e)
        return UAM_ERR_NOMEM;
    e->pc = blk->guest_pc;
    e->blk = blk;
    e->priv = priv;
    e->mapped = mapped;
    e->xlate_gen = xlate_gen;
    e->next = *head;
    *head = e;
    tc->n_blocks++;
    return UAM_OK;
}
