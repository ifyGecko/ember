/*
 * test_host.c - Unit tests for the host side and its interfaces: chardev
 * buffering and flow control, the 16550 over a chardev, netdev pairs and
 * pcap capture, the host loop (idle hart woken by socket input, deadlock
 * detection), the board registry and board configuration lookups.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "uam/boards.h"
#include "uam/dev/uart16550.h"
#include "uam/host.h"
#include "uam/isa/mips32.h"
#include "uam/machine.h"
#include "uam/signal.h"

static int n_checks, n_failed;

#define CHECK(cond, ...) do {                                              \
        n_checks++;                                                        \
        if (!(cond)) {                                                     \
            n_failed++;                                                    \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);           \
            fprintf(stderr, __VA_ARGS__);                                  \
            fputc('\n', stderr);                                           \
        }                                                                  \
    } while (0)

/* ---- chardev flow control ---------------------------------------------- */

typedef struct sink {
    uint8_t data[8192];
    size_t  len;
    size_t  window;   /* bytes accepted before the next accept_input */
} sink;

static size_t sink_can(void *ctx) { return ((sink *)ctx)->window; }

static void sink_rx(void *ctx, const uint8_t *buf, size_t len)
{
    sink *s = ctx;
    memcpy(s->data + s->len, buf, len);
    s->len += len;
    s->window -= len;
}

static void test_chardev_flow(void)
{
    static uint8_t input[6000];
    for (size_t i = 0; i < sizeof(input); i++)
        input[i] = (uint8_t)(i * 7);
    uam_chardev *cd = uam_chardev_buffer_create(NULL, input, sizeof(input));
    static sink s;
    s.window = 10;
    uam_chardev_set_frontend(cd, sink_can, sink_rx, &s);
    CHECK(s.len == 10, "frontend got only its window (%zu)", s.len);
    for (int i = 0; i < 2000 && s.len < sizeof(input); i++) {
        s.window = 7;
        uam_chardev_accept_input(cd);
    }
    CHECK(s.len == sizeof(input), "all input delivered (%zu)", s.len);
    CHECK(memcmp(s.data, input, sizeof(input)) == 0, "input in order, nothing lost");
    uam_chardev_destroy(cd);
}

/* ---- 16550 over a chardev ---------------------------------------------- */

static void test_uart_chardev(void)
{
    FILE *out = tmpfile();
    uam_chardev *cd = uam_chardev_buffer_create(out, "ok", 2);
    uam_device *u = uam_uart16550_create("uart", cd);
    uint64_t v = 0;
    u->ops->read(u, 5, 1, &v);
    CHECK(v & 1, "LSR.DR set by chardev input");
    u->ops->read(u, 0, 1, &v);
    CHECK(v == 'o', "RBR = first byte");
    u->ops->read(u, 0, 1, &v);
    CHECK(v == 'k', "RBR = second byte");
    u->ops->write(u, 0, 1, 'Z');
    u->ops->destroy(u);
    uam_chardev_destroy(cd);   /* flushes */
    rewind(out);
    CHECK(fgetc(out) == 'Z', "THR reaches the chardev output");
    fclose(out);
}

/* ---- netdev ------------------------------------------------------------- */

typedef struct nic {
    unsigned frames, last_len;
    bool     ready, link;
} nic;

static bool nic_can(void *ctx) { return ((nic *)ctx)->ready; }
static void nic_rx(void *ctx, const uint8_t *f, size_t len)
{
    (void)f;
    nic *n = ctx;
    n->frames++;
    n->last_len = (unsigned)len;
}
static void nic_link(void *ctx, bool up) { ((nic *)ctx)->link = up; }

static void test_netdev(const char *dir)
{
    uam_netdev *a, *b;
    CHECK(uam_netdev_pair_create(&a, &b) == UAM_OK, "pair");
    nic na = { .ready = true }, nb = { .ready = false };
    uam_netdev_set_frontend(a, nic_can, nic_rx, nic_link, &na);
    uam_netdev_set_frontend(b, nic_can, nic_rx, nic_link, &nb);
    CHECK(na.link && nb.link, "links up");

    char pcap[512];
    snprintf(pcap, sizeof(pcap), "%s/cap.pcap", dir);
    CHECK(uam_netdev_capture(a, pcap) == UAM_OK, "capture");

    uint8_t frame[60] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    CHECK(uam_netdev_send(a, frame, sizeof(frame)), "send a->b");
    CHECK(uam_netdev_send(a, frame, 42), "send a->b again");
    CHECK(nb.frames == 0, "b not ready: frames queued");
    nb.ready = true;
    uam_netdev_accept_input(b);
    CHECK(nb.frames == 2 && nb.last_len == 42, "queued frames delivered in order");
    CHECK(uam_netdev_send(b, frame, 60), "send b->a");
    CHECK(na.frames == 1, "a received");

    uam_netdev_destroy(a);     /* closes the capture */
    CHECK(!nb.link, "peer link down when the other end goes away");
    CHECK(!uam_netdev_send(b, frame, 60), "send on a dead link fails");
    uam_netdev_destroy(b);

    struct stat st;
    CHECK(stat(pcap, &st) == 0 && st.st_size == 24 + 3 * 16 + 60 + 42 + 60,
          "pcap holds 3 frames (size %lld)", (long long)st.st_size);
}

/* ---- host loop: an idle hart woken by socket input ----------------------- */

typedef struct wake_dev {
    uam_device   dev;
    uam_machine *m;
    int          got;
} wake_dev;

static size_t wake_can(void *ctx) { (void)ctx; return 1; }
static void wake_rx(void *ctx, const uint8_t *buf, size_t len)
{
    (void)len;
    wake_dev *w = ctx;
    w->got = buf[0];
    uam_machine_stop_cb(w->m, buf[0]);   /* the device ends the run */
}
static uam_status wake_read(uam_device *d, uint64_t o, unsigned s, uint64_t *v)
{ (void)d; (void)o; (void)s; *v = 0; return UAM_OK; }
static uam_status wake_write(uam_device *d, uint64_t o, unsigned s, uint64_t v)
{ (void)d; (void)o; (void)s; (void)v; return UAM_OK; }
static void wake_destroy(uam_device *d) { free(d); }
static const uam_device_ops wake_ops = { .type = "test,wake", .read = wake_read,
                                         .write = wake_write, .destroy = wake_destroy };

/* A MIPS machine that executes `wait` forever with no timer armed. */
static void waiting_machine(uam_machine *m)
{
    CHECK(uam_machine_init(m, &uam_isa_mips32be) == UAM_OK, "init");
    CHECK(uam_bus_map_ram(&m->bus, "ram", 0, 1 << 20) == UAM_OK, "ram");
    static const uint8_t prog[] = { 0x42, 0x00, 0x00, 0x20,   /* wait */
                                    0x10, 0x00, 0xFF, 0xFE,   /* b wait */
                                    0x00, 0x00, 0x00, 0x00 };
    uam_bus_load_bytes(&m->bus, 0, prog, sizeof(prog));
    uam_machine_reset(m, 0x80000000u);
}

static void test_host_loop(const char *dir)
{
    /* No host: waiting with nothing pending is a deadlock. */
    uam_machine m;
    waiting_machine(&m);
    CHECK(uam_exec_run(&m.exec, 1000) == UAM_STOP_DEADLOCK, "deadlock without host");
    uam_machine_fini(&m);

    /* A host loop with no fds cannot wake the hart either. */
    waiting_machine(&m);
    uam_host *h = uam_host_create();
    uam_host_attach(h, &m.exec);
    CHECK(uam_exec_run(&m.exec, 1000) == UAM_STOP_DEADLOCK, "deadlock with an fd-less host");
    uam_host_destroy(h);
    uam_machine_fini(&m);

    /* A socket chardev: the idle hart blocks in the host loop until input. */
    char path[512], spec[520];
    snprintf(path, sizeof(path), "%s/console.sock", dir);
    snprintf(spec, sizeof(spec), "unix:%s", path);
    uam_chardev *cd = uam_chardev_open(spec);
    CHECK(cd != NULL, "unix socket chardev");
    if (!cd)
        return;
    waiting_machine(&m);
    wake_dev *w = calloc(1, sizeof(*w));
    uam_device_init(&w->dev, &wake_ops, "wake");
    w->m = &m;
    uam_machine_add_device(&m, &w->dev);
    uam_chardev_set_frontend(cd, wake_can, wake_rx, w);

    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (strlen(path) < sizeof(a.sun_path))
        memcpy(a.sun_path, path, strlen(path) + 1);
    CHECK(connect(c, (struct sockaddr *)&a, sizeof(a)) == 0, "client connects");
    CHECK(write(c, "!", 1) == 1, "client writes");

    h = uam_host_create();
    uam_host_add_chardev(h, cd);
    uam_host_attach(h, &m.exec);
    uam_stop_reason why = uam_exec_run(&m.exec, 1000);
    CHECK(why == UAM_STOP_REQUEST && w->got == '!', "host input woke the run (why=%d got=%d)",
          why, w->got);
    uam_host_destroy(h);
    uam_chardev_set_frontend(cd, NULL, NULL, NULL);
    uam_machine_fini(&m);
    uam_chardev_destroy(cd);
    close(c);
}

/* ---- registry and board configuration ---------------------------------- */

static void test_registry(void)
{
    CHECK(uam_board_find("virt") == NULL, "library has no boards by default");
    uam_boards_register_stock();
    CHECK(uam_board_find("virt") == &uam_board_virt, "stock boards registered");
    CHECK(uam_board_find("mipssim-el") != NULL, "mipssim-el registered");
    CHECK(uam_board_register(&uam_board_virt) == UAM_OK, "re-registering replaces");

    uam_board_option opts[] = { { "boot", "flash" }, { "boot", "ram" } };
    uam_board_image imgs[] = { { "flash", "fw.bin" } };
    uam_board_config cfg = { .options = opts, .n_options = 2, .images = imgs, .n_images = 1 };
    CHECK(!strcmp(uam_board_config_option(&cfg, "boot"), "ram"), "last option wins");
    CHECK(uam_board_config_option(&cfg, "x") == NULL, "missing option");
    CHECK(!strcmp(uam_board_config_image(&cfg, "flash"), "fw.bin"), "named image");
}

int main(void)
{
    char dir[] = "/tmp/uam-test-XXXXXX";
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }
    test_chardev_flow();
    test_uart_chardev();
    test_netdev(dir);
    test_host_loop(dir);
    test_registry();

    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "could not remove %s\n", dir);
    printf("test_host: %d/%d checks passed\n", n_checks - n_failed, n_checks);
    return n_failed ? 1 : 0;
}
