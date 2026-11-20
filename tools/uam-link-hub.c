/*
 * uam-link-hub - A link hub as a process (uam/host.h, docs/link.md): emulations
 * connect their link ports to it, and it routes their messages over named
 * buses and, if asked, keeps their clocks together.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uam/host.h"

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

static void usage(FILE *f)
{
    fprintf(f,
        "usage: uam-link-hub LISTEN [options]\n"
        "  LISTEN               unix:PATH or tcp:[HOST:]PORT (HOST defaults to 127.0.0.1)\n"
        "  --sync MODE          how time is kept between the nodes:\n"
        "                         free         not at all; messages arrive when they arrive (default)\n"
        "                         loose[=TIME] no node runs more than TIME ahead (default 1ms);\n"
        "                                      not deterministic\n"
        "                         exact        lockstep: every message is delivered at its time,\n"
        "                                      runs are deterministic; every port needs a latency\n"
        "  --nodes N            start together: every node waits at the hub until N have joined,\n"
        "                       so that none sends to a bus the others are not on yet, or runs\n"
        "                       ahead alone (default: each starts as it connects)\n"
        "  --bus NAME=MEDIUM    how bus NAME shares its medium (a bus nobody declares is broadcast):\n"
        "                         broadcast         every other port gets every message\n"
        "                         p2p               the same; a third port is refused\n"
        "                         arbitrated[,slot=TIME]\n"
        "                                           messages for the same time contend, the lowest\n"
        "                                           key wins, the others return marked lost; times\n"
        "                                           are rounded up to the slot\n"
        "  --unit UNIT          the time unit: ps (default), ns, us or a number of units per second;\n"
        "                       64 bits of picoseconds are 213 days of emulated time\n"
        "  --keep               go on serving when the last node has left\n"
        "  --log                print joins, leaves and every message routed (stderr)\n"
        "  TIME is a number with ps, ns, us, ms or s.\n");
}

static uint64_t to_units(uint64_t ps, uint64_t unit_hz)
{
    unsigned __int128 u = ((unsigned __int128)ps * unit_hz + UAM_LINK_UNIT_HZ - 1) / UAM_LINK_UNIT_HZ;
    return u > UINT64_MAX ? UINT64_MAX : (uint64_t)u;
}

int main(int argc, char **argv)
{
    const char *listen_spec = NULL;
    uam_link_hub_config cfg = { .sync = UAM_LINK_FREE, .unit_hz = UAM_LINK_UNIT_HZ };
    uint64_t skew_ps = 1000000000u;
    const char *buses[64];
    unsigned n_buses = 0;
    bool keep = false, log = false;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(a, "--keep")) {
            keep = true;
        } else if (!strcmp(a, "--log")) {
            log = true;
        } else if (!strcmp(a, "--sync") && v) {
            i++;
            if (!strcmp(v, "free")) {
                cfg.sync = UAM_LINK_FREE;
            } else if (!strcmp(v, "exact")) {
                cfg.sync = UAM_LINK_EXACT;
            } else if (!strncmp(v, "loose", 5) && (!v[5] || (v[5] == '=' && uam_link_parse_time(v + 6, &skew_ps)))) {
                cfg.sync = UAM_LINK_LOOSE;
            } else {
                fprintf(stderr, "uam-link-hub: --sync %s: use free, loose[=TIME] or exact\n", v);
                return 2;
            }
        } else if (!strcmp(a, "--nodes") && v) {
            i++;
            cfg.start_members = (unsigned)strtoul(v, NULL, 0);
        } else if (!strcmp(a, "--unit") && v) {
            i++;
            if (!strcmp(v, "ps"))
                cfg.unit_hz = UAM_LINK_UNIT_HZ;
            else if (!strcmp(v, "ns"))
                cfg.unit_hz = 1000000000u;
            else if (!strcmp(v, "us"))
                cfg.unit_hz = 1000000u;
            else if (!(cfg.unit_hz = strtoull(v, NULL, 0))) {
                fprintf(stderr, "uam-link-hub: --unit %s: use ps, ns, us or units per second\n", v);
                return 2;
            }
        } else if (!strcmp(a, "--bus") && v && n_buses < 64) {
            i++;
            buses[n_buses++] = v;
        } else if (a[0] != '-' && !listen_spec) {
            listen_spec = a;
        } else {
            fprintf(stderr, "uam-link-hub: unexpected argument %s\n", a);
            usage(stderr);
            return 2;
        }
    }
    if (!listen_spec) {
        usage(stderr);
        return 2;
    }
    cfg.skew = to_units(skew_ps, cfg.unit_hz);

    uam_link_hub *hub = uam_link_hub_create(&cfg);
    if (!hub)
        return 1;
    for (unsigned i = 0; i < n_buses; i++) {
        char name[UAM_LINK_NAME_MAX];
        const char *eq = strchr(buses[i], '=');
        uam_link_medium medium = UAM_LINK_BROADCAST;
        uint64_t slot_ps = 0;
        bool ok = eq && eq != buses[i] && (size_t)(eq - buses[i]) < sizeof(name);
        if (ok) {
            memcpy(name, buses[i], (size_t)(eq - buses[i]));
            name[eq - buses[i]] = 0;
            const char *m = eq + 1;
            if (!strcmp(m, "broadcast"))
                medium = UAM_LINK_BROADCAST;
            else if (!strcmp(m, "p2p"))
                medium = UAM_LINK_POINT_TO_POINT;
            else if (!strcmp(m, "arbitrated"))
                medium = UAM_LINK_ARBITRATED;
            else if (!strncmp(m, "arbitrated,slot=", 16) && uam_link_parse_time(m + 16, &slot_ps))
                medium = UAM_LINK_ARBITRATED;
            else
                ok = false;
        }
        if (!ok || uam_link_hub_add_bus(hub, name, medium, to_units(slot_ps, cfg.unit_hz)) != UAM_OK) {
            fprintf(stderr, "uam-link-hub: --bus %s: use NAME=broadcast, NAME=p2p or "
                    "NAME=arbitrated[,slot=TIME], each bus once\n", buses[i]);
            return 2;
        }
    }
    uam_link_server *srv = uam_link_server_create(hub, listen_spec);
    if (!srv)
        return 1;
    if (log)
        uam_link_server_set_log(srv, stderr);
    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    while (!stop)
        if (!uam_link_server_step(srv, 1000) && !keep)
            break;
    if (log)
        fprintf(stderr, "hub: %llu message(s) routed\n",
                (unsigned long long)uam_link_server_messages(srv));
    uam_link_server_destroy(srv);
    uam_link_hub_destroy(hub);
    return 0;
}
