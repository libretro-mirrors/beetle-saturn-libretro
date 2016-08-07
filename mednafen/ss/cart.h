/******************************************************************************/
/* Mednafen Sega Saturn Emulation Module                                      */
/******************************************************************************/
/* cart.h - Expansion cart emulation
**  Copyright (C) 2016 Mednafen Team
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

#ifndef __MDFN_SS_CART_H
#define __MDFN_SS_CART_H

namespace MDFN_IEN_SS
{

struct CartInfo
{
 void (*Reset)(bool powering_up);
 void (*Kill)(void);

 // A >> 20

 struct
 {
  void (*Read16)(uint32 A, uint16* DB);
  void (*Write8)(uint32 A, uint16* DB);
  void (*Write16)(uint32 A, uint16* DB);  
 } CS0_RW[0x20];

 struct
 {
  void (*Read16)(uint32 A, uint16* DB);
  void (*Write8)(uint32 A, uint16* DB);
  void (*Write16)(uint32 A, uint16* DB);  
 } CS1_RW[0x10];
};

extern CartInfo Cart;

static INLINE void CART_CS0_Read16_DB(uint32 A, uint16* DB)  { Cart.CS0_RW[(size_t)(A >> 20) - (0x02000000 >> 20)].Read16 (A, DB); }
static INLINE void CART_CS0_Write8_DB(uint32 A, uint16* DB)  { Cart.CS0_RW[(size_t)(A >> 20) - (0x02000000 >> 20)].Write8 (A, DB); }
static INLINE void CART_CS0_Write16_DB(uint32 A, uint16* DB) { Cart.CS0_RW[(size_t)(A >> 20) - (0x02000000 >> 20)].Write16(A, DB); }

static INLINE void CART_CS1_Read16_DB(uint32 A, uint16* DB)  { Cart.CS1_RW[(size_t)(A >> 20) - (0x04000000 >> 20)].Read16 (A, DB); }
static INLINE void CART_CS1_Write8_DB(uint32 A, uint16* DB)  { Cart.CS1_RW[(size_t)(A >> 20) - (0x04000000 >> 20)].Write8 (A, DB); }
static INLINE void CART_CS1_Write16_DB(uint32 A, uint16* DB) { Cart.CS1_RW[(size_t)(A >> 20) - (0x04000000 >> 20)].Write16(A, DB); }

enum
{
 CART__RESERVED = -1,
 CART_NONE = 0,
 CART_BACKUP_MEM,
 CART_EXTRAM_1M,
 CART_EXTRAM_4M,

 CART_KOF95,
 CART_ULTRAMAN,

 CART_MDFN_DEBUG
};


void CART_Init(const int cart_type) MDFN_COLD;
void CART_Kill(void) MDFN_COLD;
void CART_GetNVInfo(const char** ext, void** nv_ptr, uint64* nv_size) MDFN_COLD;
bool CART_GetClearNVDirty(void);
void CART_Reset(bool powering_up) MDFN_COLD;


}
#endif
