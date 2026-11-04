/*
 * uam/dev/mcs51_timer.h - The 8051's Timer 0 and Timer 1 and the TCON register they
 * share with the external interrupts INT0 and INT1: the timer block every 8051 has.
 *
 * Register map (offsets from base; map it at SFR 0x88), bytes:
 *   0 TCON  TF1 TR1 TF0 TR0 IE1 IT1 IE0 IT0
 *   1 TMOD  GATE C/T M1 M0 (Timer 1, high nibble; Timer 0, low nibble)
 *   2 TL0   3 TL1   4 TH0   5 TH1
 * Modes 0 (13 bits), 1 (16 bits), 2 (8 bits, reloaded from THx) and 3 (Timer 0 as two
 * 8-bit timers, TH0 run by TR1 and setting TF1; Timer 1 stopped). A timer counts one
 * per tick of virtual time (one per instruction) while TRx is set and, with GATE, INTx
 * is high; as a counter (C/T) it counts the falling edges of its T pin, which nothing
 * drives here, so it does not count. INTx: with ITx set a falling edge sets IEx, which
 * taking the interrupt clears; with ITx clear IEx follows the pin (low = request).
 *
 * Interrupt lines, in the 8051's source order: uam_mcs51_timer_irq(dev, 0) IE0,
 * 1 TF0, 2 IE1, 3 TF1. Connect line k to the core's source k and pass taken sources to
 * uam_mcs51_timer_ack (from the core's uam_mcs51_set_irq_ack callback), which clears
 * TF0/TF1 and edge-triggered IE0/IE1 as the 8051 does.
 */
#ifndef UAM_DEV_MCS51_TIMER_H
#define UAM_DEV_MCS51_TIMER_H

#include "uam/device.h"
#include "uam/sched.h"
#include "uam/signal.h"

#define UAM_MCS51_TIMER_SIZE 6u

uam_device *uam_mcs51_timer_create(const char *name, uam_sched *sched);
/* Source k (0 IE0, 1 TF0, 2 IE1, 3 TF1). */
uam_signal *uam_mcs51_timer_irq(uam_device *dev, unsigned k);
/* The core took source k (0..3; others are ignored). */
void        uam_mcs51_timer_ack(uam_device *dev, unsigned k);
/* Drive pin INTn (n = 0, 1): level 1 high (idle), 0 low (asserted). For uam_signal
 * connections the input is the pin number. */
void        uam_mcs51_timer_int_pin(void *dev, unsigned n, int level);

#endif /* UAM_DEV_MCS51_TIMER_H */
