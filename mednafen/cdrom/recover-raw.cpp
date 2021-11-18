/*  dvdisaster: Additional error correction for optical media.
 *  Copyright (C) 2004-2007 Carsten Gnoerlich.
 *  Project home page: http://www.dvdisaster.com
 *  Email: carsten@dvdisaster.com  -or-  cgnoerlich@fsfe.org
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA,
 *  or direct your browser at http://www.gnu.org.
 */

#include <boolean.h>
#include "dvdisaster.h"

static GaloisTables *gt = NULL;		/* for L-EC Reed-Solomon */
static ReedSolomonTables *rt = NULL;

bool Init_LEC_Correct(void)
{
 gt = CreateGaloisTables(0x11d);
 rt = CreateReedSolomonTables(gt, 0, 1, 10);

 return(1);
}

void Kill_LEC_Correct(void)
{
 FreeGaloisTables(gt);
 FreeReedSolomonTables(rt);
}

/***
 *** CD level CRC calculation
 ***/

/*
 * Test raw sector against its 32bit CRC.
 * Returns true if frame is good.
 */

int CheckEDC(const unsigned char *cd_frame, bool xa_mode)
{ 
 unsigned int expected_crc, real_crc;
 unsigned int crc_base = xa_mode ? 2072 : 2064;

 expected_crc  = cd_frame[crc_base + 0] << 0;
 expected_crc |= cd_frame[crc_base + 1] << 8;
 expected_crc |= cd_frame[crc_base + 2] << 16;
 expected_crc |= cd_frame[crc_base + 3] << 24;

 if(xa_mode) 
  real_crc = EDCCrc32(cd_frame+16, 2056);
 else
  real_crc = EDCCrc32(cd_frame, 2064);

 if(expected_crc == real_crc)
  return 1;
 return 0;
}

/***
 *** Validate CD raw sector
 ***/

int ValidateRawSector(unsigned char *frame, bool xaMode)
{  
  /* Do simple L-EC.
     It seems that drives stop their internal L-EC as soon as the
     EDC is okay, so we may see uncorrected errors in the parity bytes.
     Since we are also interested in the user data only and doing the
     L-EC is expensive, we skip our L-EC as well when the EDC is fine. */

  if(!CheckEDC(frame, xaMode))
  {
   unsigned char header[4];

   if(xaMode)
   {
    memcpy(header, frame + 12, 4);
    memset(frame + 12, 0, 4);
   }

   if(xaMode)
    memcpy(frame + 12, header, 4);
  }

  /* Test internal sector checksum again */
  /* EDC failure in RAW sector */
  if(!CheckEDC(frame, xaMode))
   return false;
  return true;
}

