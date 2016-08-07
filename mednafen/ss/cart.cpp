/******************************************************************************/
/* Mednafen Sega Saturn Emulation Module                                      */
/******************************************************************************/
/* cart.cpp - Expansion cart emulation
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

#include "ss.h"
#include <mednafen/mednafen.h>
#include <mednafen/FileStream.h>
#include <mednafen/endian.h>
#include <mednafen/settings.h>
#include <mednafen/general.h>

#include "cart.h"

namespace MDFN_IEN_SS
{

static uint16 ExtRAM[0x200000];	// Also used for cart ROM
static size_t ExtRAM_Mask;

static uint8 ExtBackupRAM[0x80000];
static bool ExtBackupRAM_Dirty;

static uint8 Cart_ID;

static int CartType;
CartInfo Cart;

template<typename T, bool IsWrite>
static void Debug_RW_DB(uint32 A, uint16* DB)
{
 //
 // printf-related debugging
 //
 if((A &~ 0x3) == 0x02100000)
 {
  if(IsWrite)
  {
   if(A == 0x02100001)
   {
    fputc(*DB, stderr);
    fflush(stderr);
   }
  }
  else
   *DB = 0;

  return;
 }
}


static MDFN_HOT void CartID_Read_DB(uint32 A, uint16* DB)
{
 if((A & ~1) == 0x04FFFFFE)
  *DB = Cart_ID;
}

template<typename T, bool IsWrite>
static MDFN_HOT void ExtRAM_RW_DB(uint32 A, uint16* DB)
{
 const uint32 mask = (sizeof(T) == 2) ? 0xFFFF : (0xFF << (((A & 1) ^ 1) << 3));
 uint16* const ptr = (uint16*)((uint8*)ExtRAM + (A & ExtRAM_Mask));

 //printf("Barf %zu %d: %08x\n", sizeof(T), IsWrite, A);

 if(IsWrite)
  *ptr = (*ptr & ~mask) | (*DB & mask);
 else
  *DB = *ptr;
}

// TODO: Check mirroring.
template<typename T, bool IsWrite>
static MDFN_HOT void ExtBackupRAM_RW_DB(uint32 A, uint16* DB)
{
 uint8* const ptr = ExtBackupRAM + ((A >> 1) & 0x7FFFF);

 if(IsWrite)
 {
  if(A & 1)
  {
   ExtBackupRAM_Dirty = true;
   *ptr = *DB;
  }
 }
 else
 {
  *DB = (*ptr << 0) | 0xFF00;

  if((A & ~1) == 0x04FFFFFE)
   *DB = 0x21;
 }
}

static MDFN_HOT void ROM_Read(uint32 A, uint16* DB)
{
 // TODO: Check mirroring.
 //printf("ROM: %08x\n", A);
 *DB = *(uint16*)((uint8*)ExtRAM + (A & ExtRAM_Mask));
}

template<typename T>
static MDFN_HOT void DummyRead(uint32 A, uint16* DB)
{
 SS_DBG(SS_DBG_WARNING, "[CART] Unknown %zu-byte read from 0x%08x\n", sizeof(T), A);
}

template<typename T>
static MDFN_HOT void DummyWrite(uint32 A, uint16* DB)
{
 SS_DBG(SS_DBG_WARNING, "[CART] Unknown %zu-byte write to 0x%08x(DB=0x%04x)\n", sizeof(T), A, *DB);
}

void CART_Reset(bool powering_up)
{
 if(powering_up)
 {
  if(CartType == CART_EXTRAM_1M || CartType == CART_EXTRAM_4M)
   memset(ExtRAM, 0, sizeof(ExtRAM));	// TODO: Test.
 }
}

void CART_Init(const int cart_type)
{
 CartType = cart_type;

 for(auto& p : Cart.CS0_RW)
 {
  p.Read16 = DummyRead<uint16>;
  p.Write8 = DummyWrite<uint8>;
  p.Write16 = DummyWrite<uint16>;
 }

 for(auto& p : Cart.CS1_RW)
 {
  p.Read16 = DummyRead<uint16>;
  p.Write8 = DummyWrite<uint8>;
  p.Write16 = DummyWrite<uint16>;
 }

 if(cart_type == CART_NONE)
 {

 }
 else if(cart_type == CART_KOF95 || cart_type == CART_ULTRAMAN)
 {
  const std::string path = MDFN_MakeFName(MDFNMKF_FIRMWARE, 0, MDFN_GetSettingS((cart_type == CART_KOF95) ? "ss.cart.kof95_path" : "ss.cart.ultraman_path"));
  FileStream fp(path, FileStream::MODE_READ);

  fp.read(ExtRAM, 0x200000);

  for(unsigned i = 0; i < 0x100000; i++)
  {
   ExtRAM[i] = MDFN_de16msb<true>(&ExtRAM[i]);
  }

  ExtRAM_Mask = 0x1FFFFE;

  SS_SetPhysMemMap(0x02000000, 0x03FFFFFF, ExtRAM, 0x200000, false);

  for(uint32 A = 0x02000000; A < 0x04000000; A += (1U << 20))
  {
   auto& cs0rw = Cart.CS0_RW[(A - 0x02000000) >> 20];

   cs0rw.Read16 = ROM_Read;
  }
 }
 else if(cart_type == CART_BACKUP_MEM)
 {
  static const uint8 init[0x10] = { 0x42, 0x61, 0x63, 0x6B, 0x55, 0x70, 0x52, 0x61, 0x6D, 0x20, 0x46, 0x6F, 0x72, 0x6D, 0x61, 0x74 };
  memset(ExtBackupRAM, 0x00, sizeof(ExtBackupRAM));
  for(unsigned i = 0; i < 0x200; i += 0x10)
   memcpy(ExtBackupRAM + i, init, 0x10);

  ExtBackupRAM_Dirty = false;

  for(uint32 A = 0x04000000; A < 0x05000000; A += (1U << 20))
  {
   auto& cs1rw = Cart.CS1_RW[(A - 0x04000000) >> 20];

   cs1rw.Read16 = ExtBackupRAM_RW_DB<uint16, false>;
   cs1rw.Write8 = ExtBackupRAM_RW_DB<uint8, true>;
   cs1rw.Write16 = ExtBackupRAM_RW_DB<uint16, true>;
  }
 }
 else if(cart_type == CART_EXTRAM_1M || cart_type == CART_EXTRAM_4M)
 {
  if(cart_type == CART_EXTRAM_4M)
  {
   Cart_ID = 0x5C;
   ExtRAM_Mask = 0x3FFFFE;
  }
  else
  {
   Cart_ID = 0x5A;
   ExtRAM_Mask = 0x27FFFE;
  }

  SS_SetPhysMemMap(0x02400000, 0x025FFFFF, ExtRAM + (0x000000 / sizeof(uint16)), ((cart_type == CART_EXTRAM_4M) ? 0x200000 : 0x080000), true);
  SS_SetPhysMemMap(0x02600000, 0x027FFFFF, ExtRAM + (0x200000 / sizeof(uint16)), ((cart_type == CART_EXTRAM_4M) ? 0x200000 : 0x080000), true);

  for(uint32 A = 0x02400000; A < 0x02800000; A += (1U << 20))
  {
   auto& cs0rw = Cart.CS0_RW[(A - 0x02000000) >> 20];

   cs0rw.Read16 = ExtRAM_RW_DB<uint16, false>;
   cs0rw.Write8 = ExtRAM_RW_DB<uint8, true>;
   cs0rw.Write16 = ExtRAM_RW_DB<uint16, true>;
  }

  for(uint32 A = 0x04FFFFFE; A < 0x05000000; A += (1U << 20))
  {
   auto& cs1rw = Cart.CS1_RW[(A - 0x04000000) >> 20];

   cs1rw.Read16 = CartID_Read_DB;
  }
 }
 else if(cart_type == CART_MDFN_DEBUG)
 {
  for(uint32 A = 0x02100000; A < 0x02100002; A += (1U << 20))
  {
   auto& cs0rw = Cart.CS0_RW[(A - 0x02000000) >> 20];

   cs0rw.Read16 = Debug_RW_DB<uint16, false>;
   cs0rw.Write8 = Debug_RW_DB<uint8, true>;
   cs0rw.Write16 = Debug_RW_DB<uint16, true>;
  }
 }
 else
  abort();
}

bool CART_GetClearNVDirty(void)
{
 if(CartType == CART_BACKUP_MEM)
 {
  bool ret = ExtBackupRAM_Dirty;
  ExtBackupRAM_Dirty = false;
  return ret;
 }
 else
  return false;
}

void CART_GetNVInfo(const char** ext, void** nv_ptr, uint64* nv_size)
{
 *ext = nullptr;
 *nv_ptr = nullptr;
 *nv_size = 0;

 if(CartType == CART_BACKUP_MEM)
 {
  *ext = "bcr";
  *nv_ptr = ExtBackupRAM;
  *nv_size = sizeof(ExtBackupRAM);
 }
}

void CART_Kill(void)
{

}

}
