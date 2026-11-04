/*
 * uam/dev/z80_ctc.h - Z80 CTC: four counter/timer channels that interrupt in the Z80's
 * mode 2, with their own vector and the daisy-chain priority (channel 0 highest).
 *
 * Registers: one byte per channel (offsets 0-3). A write is
 *   - the time constant, when the last control word had bit 2 set (0 means 256)
 *   - a control word when bit 0 is 1: bit 7 interrupt enable, bit 6 counter mode (else
 *     timer), bit 5 prescaler 256 (else 16), bit 3 timer started by a trigger (else by the
 *     time constant), bit 2 a time constant follows, bit 1 software reset (stop)
 *   - the vector when bit 0 is 0 (channel 0 only): bits 7-3; a channel's vector has its
 *     number in bits 2-1.
 * A read returns the channel's down-counter.
 *
 * Time: a timer counts one per 16 or 256 ticks of virtual time (instructions on a Z80
 * board); a counter counts uam_z80_ctc_trigger() calls. At zero the channel reloads and,
 * with bit 7 set, requests an interrupt. The line is high while a request is pending that
 * no channel of higher or equal priority being serviced blocks. The board answers the
 * CPU's interrupt acknowledge with uam_z80_ctc_ack(), which returns the vector and puts the
 * channel in service, and tells the CTC of each RETI (uam_z80_ctc_reti), which ends the
 * service of the highest-priority channel. Not modelled: the ZC/TO outputs and the
 * trigger edge selection (bit 4).
 */
#ifndef UAM_DEV_Z80_CTC_H
#define UAM_DEV_Z80_CTC_H

#include "uam/device.h"
#include "uam/sched.h"
#include "uam/signal.h"

#define UAM_Z80_CTC_SIZE 4u

uam_device *uam_z80_ctc_create(const char *name, uam_sched *sched);
uam_signal *uam_z80_ctc_irq(uam_device *dev);
/* True while the CTC asks for the interrupt (its line is high). */
bool        uam_z80_ctc_pending(uam_device *dev);
/* Interrupt acknowledge: the vector of the channel taken, which goes in service. */
uint8_t     uam_z80_ctc_ack(uam_device *dev);
void        uam_z80_ctc_reti(uam_device *dev);
/* An edge on channel ch's CLK/TRG input. */
void        uam_z80_ctc_trigger(uam_device *dev, unsigned ch);

#endif /* UAM_DEV_Z80_CTC_H */
