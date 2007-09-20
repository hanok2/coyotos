/*
 * Copyright (C) 2007, The EROS Group, LLC.
 *
 * This file is part of the Coyotos Operating System.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/** @file
 * @brief I/O APIC support.
 */

#include <stddef.h>
#include <stdbool.h>
#include <hal/kerntypes.h>
#include <hal/irq.h>
#include <hal/machine.h>
#include <kerninc/printf.h>
#include <kerninc/assert.h>
#include <kerninc/CPU.h>
#include <kerninc/PhysMem.h>
#include <kerninc/vector.h>
#include <coyotos/i386/io.h>
#include "IRQ.h"
#include "PIC.h"
#include "8259.h"
#include "cpu.h"
#include "acpi.h"
#include "ioapic.h"
#include "lapic.h"

bool lapic_requires_8259_disable = false;

kpa_t lapic_pa = 0;		/* if present, certainly won't be here */
kva_t lapic_va = 0;

#define DEBUG_IOAPIC if (0)

static void ioapic_setup(VectorInfo *vector);
static void ioapic_enable(VectorInfo *vector);
static void ioapic_disable(VectorInfo *vector);
static void ioapic_earlyAck(VectorInfo *vector);
static void ioapic_lateAck(VectorInfo *vector);

// static void ioapic_acknowledge(IrqController *ctrlr, irq_t irq);
static bool ioapic_isPending(VectorInfo *vector);

static IrqController ioapic[3];
static size_t nIoAPIC = 0;

void
ioapic_register(irq_t baseIRQ, kva_t va)
{
  if (nIoAPIC == (sizeof(ioapic) / sizeof(ioapic[0])))
    fatal("Too many I/O APICs\n");

  ioapic[nIoAPIC].baseIRQ = baseIRQ;
  ioapic[nIoAPIC].nIRQ = 0;	/* not yet known */
  ioapic[nIoAPIC].va = va;
  ioapic[nIoAPIC].setup = ioapic_setup;
  ioapic[nIoAPIC].enable = ioapic_enable;
  ioapic[nIoAPIC].disable = ioapic_disable;
  ioapic[nIoAPIC].isPending = ioapic_isPending;
  ioapic[nIoAPIC].earlyAck = ioapic_earlyAck;
  ioapic[nIoAPIC].lateAck = ioapic_lateAck;


  nIoAPIC++;
}


static inline uint32_t
ioapic_read_reg(IrqController *ctrlr, uint32_t reg)
{
  /* data window is at ioapic_va + 0x10 */
  volatile uint32_t *va_reg = (uint32_t *) ctrlr->va;
  volatile uint32_t *va_data = (uint32_t *) (ctrlr->va + 0x10);

  *va_reg = reg;
  uint32_t val = *va_data;
  return val;
}

static inline void
ioapic_write_reg(IrqController *ctrlr, uint32_t reg, uint32_t val)
{
  /* data window is at ioapic_va + 0x10 */
  volatile uint32_t *va_reg = (uint32_t *) ctrlr->va;
  volatile uint32_t *va_data = (uint32_t *) (ctrlr->va + 0x10);

  *va_reg = reg;
  *va_data = val;
}

static inline IoAPIC_Entry
ioapic_read_entry(IrqController *ctrlr, uint32_t pin)
{
  IoAPIC_Entry ent;

  ent.u.raw.lo = ioapic_read_reg(ctrlr, IOAPIC_ENTRYLO(pin));
  ent.u.raw.hi = ioapic_read_reg(ctrlr, IOAPIC_ENTRYHI(pin));
  return ent;
}

static inline void
ioapic_write_entry(IrqController *ctrlr, uint32_t pin, IoAPIC_Entry ent)
{
  ioapic_write_reg(ctrlr, IOAPIC_ENTRYLO(pin), ent.u.raw.lo);
  ioapic_write_reg(ctrlr, IOAPIC_ENTRYHI(pin), ent.u.raw.hi);
}

/* The now-obsolete Intel Multiprocessor Specification introduces an
 * Interrupt Mode Control Register, which is used to get the chipset
 * to re-arrange the interrupt lines back and forth between the legacy
 * interrupt controller and the local APIC. Curiously, there is no
 * mention of any such requirement in later ACPI specs.
 *
 * Protocol: write the constant 0x70 to the IMCR (port 22), then write
 * the desired mode to port 23.
 */
#define IMCR 0x22
#define IMCR_DATA 0x23
#define IMCR_SET_INTERRUPT_MODE 0x70
#define IMCR_PIC_MODE 0
#define IMCR_LAPIC_MODE 1

static spinlock_t ioapic_lock;

static void
ioapic_setup(VectorInfo *vi)
{
  assert(vi->mode != VEC_MODE_FROMBUS);
  assert(vi->level != VEC_LEVEL_FROMBUS);

  SpinHoldInfo shi = spinlock_grab(&ioapic_lock);
  size_t pin = vi->irq - vi->ctrlr->baseIRQ;

  IoAPIC_Entry e = ioapic_read_entry(vi->ctrlr, pin);

  if (vi->level == VEC_LEVEL_ACTHIGH)
    e.u.fld.polarity = 0;
  else
    e.u.fld.polarity = 1;

  if (vi->mode == VEC_MODE_EDGE)
    e.u.fld.triggerMode = 0;
  else
    e.u.fld.triggerMode = 1;

  ioapic_write_entry(vi->ctrlr, pin, e);

  spinlock_release(shi);

}

static void
ioapic_enable(VectorInfo *vi)
{
  SpinHoldInfo shi = spinlock_grab(&ioapic_lock);
  size_t pin = vi->irq - vi->ctrlr->baseIRQ;

  IoAPIC_Entry e = ioapic_read_entry(vi->ctrlr, pin);
  e.u.fld.masked = 0;
  ioapic_write_entry(vi->ctrlr, pin, e);

  spinlock_release(shi);
}

static void
ioapic_disable(VectorInfo *vi)
{
  SpinHoldInfo shi = spinlock_grab(&ioapic_lock);
  size_t pin = vi->irq - vi->ctrlr->baseIRQ;

  IoAPIC_Entry e = ioapic_read_entry(vi->ctrlr, pin);
  e.u.fld.masked = 1;
  ioapic_write_entry(vi->ctrlr, pin, e);

  spinlock_release(shi);
}

static void
ioapic_earlyAck(VectorInfo *vi)
{
  if (vi->mode == VEC_MODE_EDGE)
    // If intterupt was edge triggered, issue early EOI:
    lapic_eoi();
}

static void
ioapic_lateAck(VectorInfo *vi)
{
  if (vi->mode == VEC_MODE_LEVEL)
    lapic_eoi();
}

#if 0
static void
ioapic_acknowledge(IrqController *ctrlr, irq_t irq)
{
  SpinHoldInfo shi = spinlock_grab(&ioapic_lock);

  bug("Cat can't acknowledge food in tin apics\n");

  spinlock_release(shi);
}
#endif

/**
 * There does not appear to be any way to check for interrupt
 * de-assertion on the lapic.
 */
static bool
ioapic_isPending(VectorInfo *vi)
{
  return irq_isEnabled(vi->irq);
}

static void 
ioapic_ctrlr_init(IrqController *ctrlr)
{
  uint32_t id = ioapic_read_reg(ctrlr, IOAPIC_ID);
  uint32_t ver = ioapic_read_reg(ctrlr, IOAPIC_VERSION);
  uint32_t nInts = (ver & IOAPIC_MAXREDIR_MASK) >> IOAPIC_MAXREDIR_SHIFT;

  ctrlr->nIRQ = nInts + 1;
  if (ctrlr->baseIRQ + nInts > nGlobalIRQ)
    nGlobalIRQ = ctrlr->baseIRQ + nInts;

  DEBUG_IOAPIC 
    printf("I/O APIC id is %d, ver %d, nInts %d\n", 
	   id >> IOAPIC_ID_SHIFT,
	   ver & IOAPIC_VERSION_MASK,
	   nInts);

  size_t vec = 0;

  /* Set up the vector entries and their global interrupt correspondences.  */
  for (size_t pin = 0; pin < ctrlr->nIRQ; pin++) {
    irq_t irq = ctrlr->baseIRQ + pin;
#if 0
    IntSrcOverride isovr;
    acpi_map_interrupt(irq, &isovr);
#endif

    while(VectorMap[vec].type != vt_Unbound)
      vec++;

    IrqVector[irq] = &VectorMap[vec];
    VectorMap[vec].type = vt_Interrupt;
    VectorMap[vec].mode = VEC_MODE_FROMBUS; /* all legacy IRQs are edge triggered */
    VectorMap[vec].level = VEC_LEVEL_FROMBUS; /* all legacy IRQs are active high. */
    VectorMap[vec].irq = irq;
    VectorMap[vec].enabled = 0;
    VectorMap[vec].ctrlr = ctrlr;

    irq_Disable(irq);
  }
}

void
ioapic_init()
{
  if (lapic_requires_8259_disable) {
    /* Following disables all interrupts on the primary and secondary
     * 8259. Disabling secondary shouldn't be necessary, but that
     * assumes that the ASIC emulating the 8259 is sensible.
     */
    i8259_shutdown();
  }
    
  for (size_t i = 0; i < nIoAPIC; i++)
    ioapic_ctrlr_init(&ioapic[i]);

  // Linux clears interrupts on the local APIC when switching. OpenBSD
  // does not. I suspect that Linux is doing this a defense against
  // sleep recovery. For the moment, don't do it.
    
  outb(IMCR_SET_INTERRUPT_MODE, IMCR);
  outb(IMCR_LAPIC_MODE, IMCR_DATA);

  // For each vector corresponding to a defined interrupt pin, wire
  // the pin back to that vector
  for (size_t vec = 0; vec < NUM_VECTOR; vec++) {
    if (VectorMap[vec].type != vt_Interrupt)
      continue;

    IrqController *ctrlr = VectorMap[vec].ctrlr;
    size_t pin = VectorMap[vec].irq - ctrlr->baseIRQ;

    assert(VectorMap[vec].enabled == 0);

    IoAPIC_Entry e = ioapic_read_entry(ctrlr, pin);
    e.u.fld.vector = vec;
    e.u.fld.deliverMode = 0;	/* FIXED delivery */
    e.u.fld.destMode = 0;		/* Physical destination (for now) */
    // Polarity and trigger mode not yet known.
    e.u.fld.masked = 1;
    e.u.fld.dest = archcpu_vec[0].lapic_id; /* CPU0 for now */

    ioapic_write_entry(ctrlr, pin, e);

    DEBUG_IOAPIC {
      irq_t irq = ctrlr->baseIRQ + pin;
      printf("Vector %d -> irq %d  ", e.u.fld.vector, irq);
      if ((irq % 2) == 1)
	printf("\n");

      IoAPIC_Entry e2 = ioapic_read_entry(ctrlr, pin);
      if (e2.u.fld.vector != e.u.fld.vector)
	fatal("e.vector %d e2.vector %d\n",
	      e.u.fld.vector, e2.u.fld.vector);
    }
  }
  DEBUG_IOAPIC printf("\n");

  DEBUG_IOAPIC {
    for (size_t irq = 0; irq < nGlobalIRQ; irq++) {
      VectorInfo *vector = IrqVector[irq];
      size_t pin = irq - vector->ctrlr->baseIRQ;
      IoAPIC_Entry e = ioapic_read_entry(vector->ctrlr, pin);
      printf("IRQ %3d -> vector %d  ", 
	     irq, e.u.fld.vector);
      if ((irq % 2) == 1)
	printf("\n");
    }
    if ((nGlobalIRQ % 2) == 1)
      printf("\n");

    fatal("Check map.\n");
  }
}

void
ioapic_shutdown()
{
  // NOTE: This is untested and probably does not work!
  fatal("Do not know how to perform LAPIC shutdown.\n");
  outb(IMCR_SET_INTERRUPT_MODE, IMCR);
  outb(IMCR_PIC_MODE, IMCR_DATA);
}
