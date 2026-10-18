/*
 * uam/state.h - State streams: how a component saves and restores its state.
 *
 * A machine snapshot (snapshot.h) is assembled from the state of each
 * component: the hart, the ISA module, the scheduler, every device, and any
 * extra state a board registers. Each component writes its own state into a
 * uam_state_writer and reads it back from a uam_state_reader, field by field,
 * in the same order:
 *
 *     static void my_save(uam_device *dev, uam_state_writer *w)
 *     {
 *         my_dev *d = (my_dev *)dev;
 *         uam_state_put_u32(w, d->ctrl);
 *         uam_state_put_bytes(w, d->fifo, sizeof(d->fifo));
 *         uam_state_put_timer(w, &d->timer);
 *         uam_state_put_signal(w, &d->irq);
 *     }
 *     static void my_restore(uam_device *dev, uam_state_reader *r)
 *     {
 *         my_dev *d = (my_dev *)dev;
 *         d->ctrl = uam_state_get_u32(r);
 *         uam_state_get_bytes(r, d->fifo, sizeof(d->fifo));
 *         uam_state_get_timer(r, &d->timer);
 *         uam_state_get_signal(r, &d->irq);
 *     }
 *
 * Values are stored little-endian with fixed widths, so a stream written on
 * one host reads back on any other. Errors are sticky: after a short read, a
 * length mismatch or uam_state_fail(), every get returns 0 and the restore is
 * reported as failed with the first error. Components therefore need no error
 * handling of their own.
 *
 * What to save: everything the guest can observe, or that decides future
 * behaviour - registers, FIFOs, latched interrupt conditions, internal
 * counters. What not to save: wiring and configuration fixed when the board
 * was built (sinks, bus and chardev pointers, sizes), host-side state
 * (backends, file handles) and diagnostics. Use uam_state_expect_u32() for
 * configuration that must match (e.g. a FIFO depth or a source count).
 *
 * Timers: uam_state_put_timer records whether a timer is armed, its deadline,
 * and its place among timers with the same deadline. uam_state_get_timer
 * defers arming until the whole snapshot is read, then arms every restored
 * timer in its original order, so ties fire exactly as they would have.
 * Taking a snapshot fails if an armed timer is not saved by its owner.
 *
 * Signals: uam_state_get_signal restores an output's level without calling
 * its sink, since the consumer restores its own view of the line.
 *
 * Dependencies: common.h, sched.h, signal.h.
 */
#ifndef UAM_STATE_H
#define UAM_STATE_H

#include "uam/common.h"
#include "uam/sched.h"
#include "uam/signal.h"

typedef struct uam_state_writer uam_state_writer;
typedef struct uam_state_reader uam_state_reader;

/* ---- writing ------------------------------------------------------------ */

void uam_state_put_u8(uam_state_writer *w, uint8_t v);
void uam_state_put_u16(uam_state_writer *w, uint16_t v);
void uam_state_put_u32(uam_state_writer *w, uint32_t v);
void uam_state_put_u64(uam_state_writer *w, uint64_t v);
void uam_state_put_bool(uam_state_writer *w, bool v);
/* A byte array; the reader must ask for exactly `len` bytes. */
void uam_state_put_bytes(uam_state_writer *w, const void *data, size_t len);
void uam_state_put_timer(uam_state_writer *w, const uam_timer *t);
void uam_state_put_signal(uam_state_writer *w, const uam_signal *s);

/* ---- reading ------------------------------------------------------------ */

uint8_t  uam_state_get_u8(uam_state_reader *r);
uint16_t uam_state_get_u16(uam_state_reader *r);
uint32_t uam_state_get_u32(uam_state_reader *r);
uint64_t uam_state_get_u64(uam_state_reader *r);
bool     uam_state_get_bool(uam_state_reader *r);
/* Fails the stream if the stored array is not exactly `len` bytes long. */
void     uam_state_get_bytes(uam_state_reader *r, void *data, size_t len);
void     uam_state_get_timer(uam_state_reader *r, uam_timer *t);
void     uam_state_get_signal(uam_state_reader *r, uam_signal *s);
/* Read a u32 that must equal `want` (fixed configuration); fails otherwise. */
void     uam_state_expect_u32(uam_state_reader *r, uint32_t want, const char *what);

/* Mark the stream as unusable (state from an incompatible configuration). */
void        uam_state_fail(uam_state_reader *r, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
bool        uam_state_ok(const uam_state_reader *r);
/* The first error, or NULL. */
const char *uam_state_error(const uam_state_reader *r);

/* ---- streams and records ------------------------------------------------- */

/*
 * Streams are created by the snapshot code; components only see them inside
 * their save/restore callbacks. The functions below are for code that stores
 * state outside a machine snapshot (and for tests).
 *
 * A stream is a sequence of named records. The writer's `sched` is the
 * scheduler whose timers are being saved (it ranks equal deadlines); the
 * reader's is the one restored timers are armed on.
 */
uam_state_writer *uam_state_writer_new(const uam_sched *sched);
void              uam_state_writer_free(uam_state_writer *w);
bool              uam_state_writer_ok(const uam_state_writer *w);  /* false: out of memory */
const uint8_t    *uam_state_writer_data(const uam_state_writer *w, size_t *len);
/* Open a record; everything put until uam_state_end belongs to it. */
void              uam_state_begin(uam_state_writer *w, const char *name);
void              uam_state_end(uam_state_writer *w);
/* Timers saved so far (to check that every armed timer has an owner). */
bool              uam_state_writer_saved_timer(const uam_state_writer *w, const uam_timer *t);

uam_state_reader *uam_state_reader_new(const uint8_t *data, size_t len, uam_sched *sched);
void              uam_state_reader_free(uam_state_reader *r);
/* Enter the next record: its name (NUL-terminated, valid while the stream
 * is) or NULL at the end of the stream or on error. */
const char       *uam_state_next(uam_state_reader *r);
/* Leave the current record; fails the stream unless it was read completely. */
void              uam_state_leave(uam_state_reader *r, const char *who);
/* Skip the rest of the current record. */
void              uam_state_skip(uam_state_reader *r);
/* Arm the timers restored so far, in their saved order. */
void              uam_state_arm_timers(uam_state_reader *r);

#endif /* UAM_STATE_H */
