/*
 * link_node.c - A link node in a machine (uam/host.h): its clock, its state in
 * the machine's snapshots, and a named input per port.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/host.h"
#include "uam/machine.h"

static void node_save(void *ctx, uam_state_writer *w)
{
    uam_link_node_save(ctx, w);
}

static void node_restore(void *ctx, uam_state_reader *r)
{
    uam_link_node_restore(ctx, r);
}

uam_status uam_link_input_deliver(void *ctx, uam_machine *m, const uint8_t *data, size_t len)
{
    (void)m;
    uam_link_port *p = ctx;
    size_t off = 0;
    /* Records: u16 length (little-endian), then that many bytes; a record that
     * runs past the end takes what is left. No complete header: one message. */
    if (len < 2) {
        uam_link_msg one = { .len = (uint32_t)len, .data = data };
        return uam_link_port_inject(p, &one, 0);
    }
    while (off + 2 <= len) {
        size_t n = data[off] | (size_t)data[off + 1] << 8;
        off += 2;
        if (n > len - off)
            n = len - off;
        uam_link_msg msg = { .len = (uint32_t)n, .data = data + off };
        uam_status st = uam_link_port_inject(p, &msg, 0);
        if (st != UAM_OK)
            return st;
        off += n;
    }
    return UAM_OK;
}

uam_status uam_link_node_bind(uam_link_node *n, uam_machine *m, uint64_t tick_hz)
{
    if (!n || !m)
        return UAM_ERR_INVALID;
    char name[UAM_LINK_NAME_MAX + 8];
    snprintf(name, sizeof(name), "link-%s", uam_link_node_name(n));
    uam_status st = uam_machine_add_state(m, name, node_save, node_restore, n);
    if (st != UAM_OK)
        return st;
    st = uam_link_node_set_clock(n, &m->sched, tick_hz ? tick_hz : 100000000u);
    for (unsigned i = 0; st == UAM_OK && i < uam_link_node_ports(n); i++) {
        uam_link_port *p = uam_link_node_port(n, i);
        if (!uam_machine_find_input(m, uam_link_port_name(p)))
            st = uam_machine_add_input(m, uam_link_port_name(p), uam_link_input_deliver, p);
    }
    return st;
}

bool uam_link_parse_time(const char *text, uint64_t *ps)
{
    static const struct { const char *unit; uint64_t mul; } units[] = {
        { "ps", 1 }, { "ns", 1000 }, { "us", 1000000 }, { "ms", 1000000000 },
        { "s", UINT64_C(1000000000000) },
    };
    char *end;
    unsigned long long v = strtoull(text, &end, 10);
    if (end == text)
        return false;
    for (size_t i = 0; i < UAM_ARRAY_LEN(units); i++)
        if (!strcmp(end, units[i].unit)) {
            if (v > UINT64_MAX / units[i].mul)
                return false;
            *ps = v * units[i].mul;
            return true;
        }
    return false;
}
