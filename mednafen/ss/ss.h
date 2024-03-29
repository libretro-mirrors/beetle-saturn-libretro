/******************************************************************************/
/* Mednafen Sega Saturn Emulation Module                                      */
/******************************************************************************/
/* ss.h:
**  Copyright (C) 2015-2016 Mednafen Team
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of the GNU General Public License
** as published by the Free Software Foundation; either version 2
** of the License, or (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software Foundation, Inc.,
** 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#ifndef __MDFN_SS_SS_H
#define __MDFN_SS_SS_H

#include "../mednafen-types.h"
#include "../math_ops.h"
#include <stdint.h>
#include <stdarg.h>

#if 1
 enum
 {
  HORRIBLEHACK_NOSH2DMALINE106	 = (1U << 0),
  HORRIBLEHACK_NOSH2DMAPENALTY   = (1U << 1),
  HORRIBLEHACK_VDP1VRAM5000FIX	 = (1U << 2),
  HORRIBLEHACK_VDP1RWDRAWSLOWDOWN= (1U << 3),
  HORRIBLEHACK_VDP1INSTANT	     = (1U << 4),
 };
 extern uint32 ss_horrible_hacks;
#endif

 enum
 {
  SS_DBG_ERROR     = (1U << 0),
  SS_DBG_WARNING   = (1U << 1),

  SS_DBG_M68K 	   = (1U << 2),

  SS_DBG_SH2  	   = (1U << 4),
  SS_DBG_SH2_REGW  = (1U << 5),
  SS_DBG_SH2_CACHE = (1U << 6),

  SS_DBG_SCU  	   = (1U << 8),
  SS_DBG_SCU_REGW  = (1U << 9),
  SS_DBG_SCU_INT   = (1U << 10),
  SS_DBG_SCU_DSP   = (1U << 11),

  SS_DBG_SMPC 	   = (1U << 12),
  SS_DBG_SMPC_REGW = (1U << 13),

  SS_DBG_BIOS	   = (1U << 15),

  SS_DBG_CDB  	   = (1U << 16),
  SS_DBG_CDB_REGW  = (1U << 17),

  SS_DBG_VDP1 	   = (1U << 20),
  SS_DBG_VDP1_REGW = (1U << 21),
  SS_DBG_VDP1_VRAMW= (1U << 22),
  SS_DBG_VDP1_FBW  = (1U << 23),

  SS_DBG_VDP2 	   = (1U << 24),
  SS_DBG_VDP2_REGW = (1U << 25),

  SS_DBG_SCSP 	   = (1U << 28),
  SS_DBG_SCSP_REGW = (1U << 29),
 };
 enum { ss_dbg_mask = 0 };

#define SS_DBG(which, format, ...)
#define SS_DBGTI SS_DBG

 template<unsigned which>
 static void SS_DBG_Wrap(const char* format, ...) noexcept
 {
 }

 typedef int32 sscpu_timestamp_t;

 class SH7095;

 extern SH7095 CPU[2];	// for smpc.cpp

 extern int32 SH7095_mem_timestamp;

 void SS_RequestMLExit(void);
 void ForceEventUpdates(const sscpu_timestamp_t timestamp);

 enum
 {
  SS_EVENT__SYNFIRST = 0,

  SS_EVENT_SH2_M_DMA,
  SS_EVENT_SH2_S_DMA,

  SS_EVENT_SCU_DMA,
  SS_EVENT_SCU_DSP,

  SS_EVENT_SMPC,

  SS_EVENT_VDP1,
  SS_EVENT_VDP2,

  SS_EVENT_CDB,

  SS_EVENT_SOUND,

  SS_EVENT_CART,

  SS_EVENT_MIDSYNC,

  SS_EVENT__SYNLAST,
  SS_EVENT__COUNT,
 };

typedef sscpu_timestamp_t (*ss_event_handler)(const sscpu_timestamp_t timestamp);

 struct event_list_entry
 {
  sscpu_timestamp_t event_time;
  event_list_entry *prev;
  event_list_entry *next;
  ss_event_handler event_handler;
 };

 extern event_list_entry events[SS_EVENT__COUNT];

 #define SS_EVENT_DISABLED_TS			0x40000000
 void SS_SetEventNT(event_list_entry* e, const sscpu_timestamp_t next_timestamp);

 // Call from init code, or power/reset code, as appropriate.
 // (length is in units of bytes, not 16-bit units)
 //
 // is_writeable is mostly for cheat stuff.
 void SS_SetPhysMemMap(uint32 Astart, uint32 Aend, uint16* ptr, uint32 length, bool is_writeable);

 void SS_Reset(bool powering_up) MDFN_COLD;

#endif
