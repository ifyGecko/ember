/*
 * i8042.c - 8042-compatible PS/2 controller, keyboard and mouse
 * (uam/dev/i8042.h).
 *
 * The controller's output buffer is a queue of bytes, each tagged with the
 * device it came from (keyboard, mouse or the controller itself); the status
 * register shows its head. Keyboard bytes are translated to scan-code set 1
 * as they enter the queue while configuration bit 6 is set, as the 8042
 * does. IRQ 1 / IRQ 12 follow the head byte and the configuration's
 * interrupt enables.
 */
#include <stdlib.h>
#include <string.h>

#include "uam/dev/i8042.h"

#define QUEUE 64u

enum { SRC_CTRL, SRC_KBD, SRC_AUX };
enum { CFG_KBD_INT = 0x01, CFG_AUX_INT = 0x02, CFG_SYS = 0x04, CFG_KBD_OFF = 0x10,
       CFG_AUX_OFF = 0x20, CFG_XLAT = 0x40 };
enum { ST_OBF = 0x01, ST_SYS = 0x04, ST_CMD = 0x08, ST_UNLOCKED = 0x10, ST_AUX = 0x20 };

typedef struct i8042 {
    uam_device dev;            /* data port; owns the state */
    uam_device cmd_view;
    uam_signal kbd_irq, aux_irq, reset, a20;
    uint8_t    q[QUEUE], qsrc[QUEUE];
    unsigned   qhead, qlen;
    uint8_t    cfg, outport, last_was_cmd;
    uint8_t    ctrl_pending;   /* controller command waiting for its data byte */
    uint8_t    kbd_pending;    /* keyboard command waiting for its argument */
    uint8_t    aux_pending;
    bool       kbd_scan, aux_report, xlat_break;
    uint8_t    kbd_set, leds;
} i8042;

static i8042 *from_dev(uam_device *d) { return UAM_CONTAINER_OF(d, i8042, dev); }
static i8042 *from_cmd(uam_device *d) { return UAM_CONTAINER_OF(d, i8042, cmd_view); }

/* The 8042's scan-code translation, set 2 -> set 1, for codes below 0x80. */
static const uint8_t xlat[128] = {
    0xff, 0x43, 0x41, 0x3f, 0x3d, 0x3b, 0x3c, 0x58, 0x64, 0x44, 0x42, 0x40, 0x3e, 0x0f, 0x29, 0x59,
    0x65, 0x38, 0x2a, 0x70, 0x1d, 0x10, 0x02, 0x5a, 0x66, 0x71, 0x2c, 0x1f, 0x1e, 0x11, 0x03, 0x5b,
    0x67, 0x2e, 0x2d, 0x20, 0x12, 0x05, 0x04, 0x5c, 0x68, 0x39, 0x2f, 0x21, 0x14, 0x13, 0x06, 0x5d,
    0x69, 0x31, 0x30, 0x23, 0x22, 0x15, 0x07, 0x5e, 0x6a, 0x72, 0x32, 0x24, 0x16, 0x08, 0x09, 0x5f,
    0x6b, 0x33, 0x25, 0x17, 0x18, 0x0b, 0x0a, 0x60, 0x6c, 0x34, 0x35, 0x26, 0x27, 0x19, 0x0c, 0x61,
    0x6d, 0x73, 0x28, 0x74, 0x1a, 0x0d, 0x62, 0x6e, 0x3a, 0x36, 0x1c, 0x1b, 0x75, 0x2b, 0x63, 0x76,
    0x55, 0x56, 0x77, 0x78, 0x79, 0x7a, 0x0e, 0x7b, 0x7c, 0x4f, 0x7d, 0x4b, 0x47, 0x7e, 0x7f, 0x6f,
    0x52, 0x53, 0x50, 0x4c, 0x4d, 0x48, 0x01, 0x45, 0x57, 0x4e, 0x51, 0x4a, 0x37, 0x49, 0x46, 0x54,
};

static void update_irqs(i8042 *k)
{
    uint8_t src = k->qlen ? k->qsrc[k->qhead] : SRC_CTRL;
    uam_signal_set(&k->kbd_irq, k->qlen && src != SRC_AUX && (k->cfg & CFG_KBD_INT));
    uam_signal_set(&k->aux_irq, k->qlen && src == SRC_AUX && (k->cfg & CFG_AUX_INT));
}

static void push_raw(i8042 *k, uint8_t b, uint8_t src)
{
    if (k->qlen == QUEUE)
        return;   /* overrun: the byte is lost */
    unsigned t = (k->qhead + k->qlen) % QUEUE;
    k->q[t] = b;
    k->qsrc[t] = src;
    k->qlen++;
    update_irqs(k);
}

/* A byte from the keyboard, translated to set 1 when enabled. */
static void push_kbd(i8042 *k, uint8_t b)
{
    if (!(k->cfg & CFG_XLAT)) {
        push_raw(k, b, SRC_KBD);
        return;
    }
    if (b == 0xF0) {             /* set-2 break prefix: folded into the next code */
        k->xlat_break = true;
        return;
    }
    uint8_t t = b < 0x80 ? xlat[b] : b == 0x83 ? 0x41 : b == 0x84 ? 0x54 : b;
    if (k->xlat_break && b != 0xE0 && b != 0xE1)
        t |= 0x80;
    if (b != 0xE0 && b != 0xE1)
        k->xlat_break = false;
    push_raw(k, t, SRC_KBD);
}

static uint8_t pop(i8042 *k)
{
    if (!k->qlen)
        return k->qhead ? k->q[(k->qhead + QUEUE - 1) % QUEUE] : 0;   /* the last byte again */
    uint8_t b = k->q[k->qhead];
    k->qhead = (k->qhead + 1) % QUEUE;
    k->qlen--;
    update_irqs(k);
    return b;
}

static void pulse_reset(i8042 *k)
{
    uam_signal_set(&k->reset, 1);
    uam_signal_set(&k->reset, 0);
}

/* ---- keyboard and mouse -------------------------------------------------- */

static void kbd_command(i8042 *k, uint8_t b)
{
    uint8_t pend = k->kbd_pending;
    k->kbd_pending = 0;
    switch (pend) {
    case 0xED: k->leds = b & 7; push_kbd(k, 0xFA); return;
    case 0xF3: push_kbd(k, 0xFA); return;
    case 0xF0:
        push_kbd(k, 0xFA);
        if (b == 0)
            push_kbd(k, k->kbd_set);
        else if (b <= 3)
            k->kbd_set = b;
        return;
    default: break;
    }
    switch (b) {
    case 0xED: case 0xF0: case 0xF3: k->kbd_pending = b; push_kbd(k, 0xFA); break;
    case 0xEE: push_kbd(k, 0xEE); break;
    case 0xF2: push_kbd(k, 0xFA); push_kbd(k, 0xAB); push_kbd(k, 0x83); break;
    case 0xF4: k->kbd_scan = true; push_kbd(k, 0xFA); break;
    case 0xF5: k->kbd_scan = false; push_kbd(k, 0xFA); break;
    case 0xF6: k->kbd_set = 2; push_kbd(k, 0xFA); break;
    case 0xFF: k->kbd_scan = true; k->kbd_set = 2; k->leds = 0; push_kbd(k, 0xFA); push_kbd(k, 0xAA); break;
    default:   push_kbd(k, 0xFA); break;   /* typematic/make-break modes and the like */
    }
}

static void aux_command(i8042 *k, uint8_t b)
{
    if (k->aux_pending) {         /* the argument of a sample-rate/resolution command */
        k->aux_pending = 0;
        push_raw(k, 0xFA, SRC_AUX);
        return;
    }
    switch (b) {
    case 0xF3: case 0xE8: k->aux_pending = b; push_raw(k, 0xFA, SRC_AUX); break;
    case 0xF2: push_raw(k, 0xFA, SRC_AUX); push_raw(k, 0x00, SRC_AUX); break;
    case 0xF4: k->aux_report = true; push_raw(k, 0xFA, SRC_AUX); break;
    case 0xF5: k->aux_report = false; push_raw(k, 0xFA, SRC_AUX); break;
    case 0xE9:                    /* status request: 3 status bytes */
        push_raw(k, 0xFA, SRC_AUX);
        push_raw(k, k->aux_report ? 0x20 : 0x00, SRC_AUX);
        push_raw(k, 0x02, SRC_AUX);
        push_raw(k, 0x64, SRC_AUX);
        break;
    case 0xFF:
        k->aux_report = false;
        push_raw(k, 0xFA, SRC_AUX);
        push_raw(k, 0xAA, SRC_AUX);
        push_raw(k, 0x00, SRC_AUX);
        break;
    default: push_raw(k, 0xFA, SRC_AUX); break;
    }
}

/* ---- controller ---------------------------------------------------------- */

static void ctrl_command(i8042 *k, uint8_t c)
{
    switch (c) {
    case 0x20: push_raw(k, k->cfg, SRC_CTRL); break;
    case 0x60: case 0xD1: case 0xD2: case 0xD3: case 0xD4: k->ctrl_pending = c; break;
    case 0xA7: k->cfg |= CFG_AUX_OFF; break;
    case 0xA8: k->cfg &= (uint8_t)~CFG_AUX_OFF; break;
    case 0xA9: push_raw(k, 0x00, SRC_CTRL); break;   /* mouse interface OK */
    case 0xAA: k->cfg |= CFG_SYS; push_raw(k, 0x55, SRC_CTRL); break;   /* self test passed */
    case 0xAB: push_raw(k, 0x00, SRC_CTRL); break;   /* keyboard interface OK */
    case 0xAD: k->cfg |= CFG_KBD_OFF; break;
    case 0xAE: k->cfg &= (uint8_t)~CFG_KBD_OFF; break;
    case 0xC0: push_raw(k, 0xBF, SRC_CTRL); break;   /* input port: not locked */
    case 0xD0: push_raw(k, k->outport, SRC_CTRL); break;
    case 0xDD: k->outport &= (uint8_t)~0x02; uam_signal_set(&k->a20, 0); break;   /* A20 off */
    case 0xDF: k->outport |= 0x02; uam_signal_set(&k->a20, 1); break;             /* A20 on */
    default:
        if (c >= 0xF0 && !(c & 1))   /* pulse output lines; bit 0 low = reset */
            pulse_reset(k);
        break;
    }
    update_irqs(k);
}

static void data_write(i8042 *k, uint8_t b)
{
    uint8_t pend = k->ctrl_pending;
    k->ctrl_pending = 0;
    switch (pend) {
    case 0x60: k->cfg = b; update_irqs(k); return;
    case 0xD1:
        k->outport = b | 0x01;
        uam_signal_set(&k->a20, (b >> 1) & 1);   /* output port bit 1: A20 gate */
        if (!(b & 1))
            pulse_reset(k);
        return;
    case 0xD2: push_kbd(k, b); return;
    case 0xD3: push_raw(k, b, SRC_AUX); return;
    case 0xD4: aux_command(k, b); return;
    default: kbd_command(k, b); return;
    }
}

static uam_status data_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)off; (void)size;
    *val = pop(from_dev(d));
    return UAM_OK;
}

static uam_status data_wr(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    (void)off; (void)size;
    i8042 *k = from_dev(d);
    k->last_was_cmd = 0;
    data_write(k, (uint8_t)v);
    return UAM_OK;
}

static uam_status cmd_read(uam_device *d, uint64_t off, unsigned size, uint64_t *val)
{
    (void)off; (void)size;
    i8042 *k = from_cmd(d);
    uint8_t st = ST_UNLOCKED | (k->cfg & CFG_SYS ? ST_SYS : 0) | (k->last_was_cmd ? ST_CMD : 0);
    if (k->qlen) {
        st |= ST_OBF;
        if (k->qsrc[k->qhead] == SRC_AUX)
            st |= ST_AUX;
    }
    *val = st;
    return UAM_OK;
}

static uam_status cmd_wr(uam_device *d, uint64_t off, unsigned size, uint64_t v)
{
    (void)off; (void)size;
    i8042 *k = from_cmd(d);
    k->last_was_cmd = 1;
    ctrl_command(k, (uint8_t)v);
    return UAM_OK;
}

void uam_i8042_key(uam_device *d, const uint8_t *codes, size_t n)
{
    i8042 *k = from_dev(d);
    if (!k->kbd_scan || (k->cfg & CFG_KBD_OFF))
        return;
    for (size_t i = 0; i < n; i++)
        push_kbd(k, codes[i]);
}

/* ---- typing ASCII -------------------------------------------------------- */

static const char plain[] = "`1234567890-=qwertyuiop[]\\asdfghjkl;'zxcvbnm,./ \n\b\t";
static const char shifted[] = "~!@#$%^&*()_+QWERTYUIOP{}|ASDFGHJKL:\"ZXCVBNM<>?";
static const uint8_t code2[] = {
    0x0E, 0x16, 0x1E, 0x26, 0x25, 0x2E, 0x36, 0x3D, 0x3E, 0x46, 0x45, 0x4E, 0x55,
    0x15, 0x1D, 0x24, 0x2D, 0x2C, 0x35, 0x3C, 0x43, 0x44, 0x4D, 0x54, 0x5B, 0x5D,
    0x1C, 0x1B, 0x23, 0x2B, 0x34, 0x33, 0x3B, 0x42, 0x4B, 0x4C, 0x52,
    0x1A, 0x22, 0x21, 0x2A, 0x32, 0x31, 0x3A, 0x41, 0x49, 0x4A,
    0x29, 0x5A, 0x66, 0x0D,
};

size_t uam_i8042_type(uam_device *d, const char *text, size_t n)
{
    size_t done = 0;
    for (; done < n; done++) {
        const char *p = strchr(plain, text[done]);
        bool shift = false;
        if (!p || !text[done]) {
            p = strchr(shifted, text[done]);
            if (!p || !text[done])
                break;
            shift = true;
            p = plain + (p - shifted);
        }
        uint8_t c = code2[p - plain];
        uint8_t seq[8];
        size_t len = 0;
        if (shift) seq[len++] = 0x12;
        seq[len++] = c;
        seq[len++] = 0xF0;
        seq[len++] = c;
        if (shift) { seq[len++] = 0xF0; seq[len++] = 0x12; }
        uam_i8042_key(d, seq, len);
    }
    return done;
}

/* ---- device -------------------------------------------------------------- */

static void i8042_reset(uam_device *d)
{
    i8042 *k = from_dev(d);
    k->qhead = k->qlen = 0;
    k->cfg = CFG_KBD_INT | CFG_SYS | CFG_AUX_OFF | CFG_XLAT;
    k->outport = 0x03;
    uam_signal_set(&k->a20, 1);
    k->last_was_cmd = 0;
    k->ctrl_pending = k->kbd_pending = k->aux_pending = 0;
    k->kbd_scan = true;
    k->aux_report = false;
    k->xlat_break = false;
    k->kbd_set = 2;
    k->leds = 0;
    update_irqs(k);
    uam_signal_set(&k->reset, 0);
}

static void i8042_destroy(uam_device *d) { free(from_dev(d)); }

static void i8042_save(uam_device *d, uam_state_writer *w)
{
    i8042 *k = from_dev(d);
    uam_state_put_bytes(w, k->q, sizeof(k->q));
    uam_state_put_bytes(w, k->qsrc, sizeof(k->qsrc));
    uam_state_put_u32(w, k->qhead);
    uam_state_put_u32(w, k->qlen);
    const uint8_t b[] = { k->cfg, k->outport, k->last_was_cmd, k->ctrl_pending, k->kbd_pending,
                          k->aux_pending, k->kbd_scan, k->aux_report, k->xlat_break, k->kbd_set,
                          k->leds };
    uam_state_put_bytes(w, b, sizeof(b));
    uam_state_put_signal(w, &k->kbd_irq);
    uam_state_put_signal(w, &k->aux_irq);
    uam_state_put_signal(w, &k->reset);
}

static void i8042_restore(uam_device *d, uam_state_reader *r)
{
    i8042 *k = from_dev(d);
    uam_state_get_bytes(r, k->q, sizeof(k->q));
    uam_state_get_bytes(r, k->qsrc, sizeof(k->qsrc));
    k->qhead = uam_state_get_u32(r) % QUEUE;
    k->qlen = uam_state_get_u32(r);
    if (k->qlen > QUEUE) k->qlen = QUEUE;
    uint8_t b[11];
    uam_state_get_bytes(r, b, sizeof(b));
    k->cfg = b[0]; k->outport = b[1]; k->last_was_cmd = b[2]; k->ctrl_pending = b[3];
    k->kbd_pending = b[4]; k->aux_pending = b[5]; k->kbd_scan = b[6]; k->aux_report = b[7];
    k->xlat_break = b[8]; k->kbd_set = b[9]; k->leds = b[10];
    k->a20.level = (k->outport >> 1) & 1;   /* its receiver keeps its own input level */
    uam_state_get_signal(r, &k->kbd_irq);
    uam_state_get_signal(r, &k->aux_irq);
    uam_state_get_signal(r, &k->reset);
}

static const uam_device_ops data_ops = {
    .type = "pc,8042-kbc", .impl_min = 1, .impl_max = 1,
    .read = data_read, .write = data_wr, .reset = i8042_reset, .destroy = i8042_destroy,
    .save = i8042_save, .restore = i8042_restore,
};
static const uam_device_ops cmd_ops = {
    .type = "pc,8042-kbc-cmd", .impl_min = 1, .impl_max = 1,
    .read = cmd_read, .write = cmd_wr, .flags = UAM_DEVICE_STATELESS,
};

uam_device *uam_i8042_create(const char *name)
{
    i8042 *k = calloc(1, sizeof(*k));
    if (!k)
        return NULL;
    uam_device_init(&k->dev, &data_ops, name);
    uam_device_init(&k->cmd_view, &cmd_ops, name);
    uam_signal_init(&k->kbd_irq);
    uam_signal_init(&k->aux_irq);
    uam_signal_init(&k->reset);
    uam_signal_init(&k->a20);
    i8042_reset(&k->dev);
    return &k->dev;
}

uam_device *uam_i8042_data(uam_device *d) { return d; }
uam_device *uam_i8042_cmd(uam_device *d)  { return &from_dev(d)->cmd_view; }
uam_signal *uam_i8042_kbd_irq(uam_device *d) { return &from_dev(d)->kbd_irq; }
uam_signal *uam_i8042_aux_irq(uam_device *d) { return &from_dev(d)->aux_irq; }
uam_signal *uam_i8042_reset_line(uam_device *d) { return &from_dev(d)->reset; }
uam_signal *uam_i8042_a20_line(uam_device *d)   { return &from_dev(d)->a20; }
