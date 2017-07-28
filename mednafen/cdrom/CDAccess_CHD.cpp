/******************************************************************************/
/* Mednafen - Multi-system Emulator                                           */
/******************************************************************************/
/* CDAccess_CHD.cpp:
**  Copyright (C) 2017 Romain Tisserand
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

#include <mednafen/mednafen.h>
#include <mednafen/general.h>

#include "CDAccess_CHD.h"

#include <limits>
#include <limits.h>
#include <map>

CDAccess_CHD::CDAccess_CHD(const std::string& path, bool image_memcache) : img_numsectors(0)
{
   Load(path, image_memcache);
}

bool CDAccess_CHD::Load(const std::string& path, bool image_memcache)
{
  chd_error err = chd_open(path.c_str(), CHD_OPEN_READ, NULL, &chd);

  /* allocate storage for sector reads */
  const chd_header *head = chd_get_header(chd);
  hunkmem = (uint8_t*)malloc(head->hunkbytes);
  oldhunk = -1;


    log_cb(RETRO_LOG_INFO, "chd_load '%s' hunkbytes=%d\n", path.c_str(), head->hunkbytes);

  int plba = 0;
  while (1) {
    int tkid = 0, frames = 0, pad = 0, pregap = 0, postgap = 0;
    char type[64], subtype[32], pgtype[32], pgsub[32];
    char tmp[512];

    err = chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, num_tracks, tmp, sizeof(tmp), NULL, NULL, NULL);
    if (err == CHDERR_NONE) {
      sscanf(tmp, CDROM_TRACK_METADATA2_FORMAT, &tkid, type, subtype, &frames, &pregap, pgtype, pgsub, &postgap);
    } else {
      /* try to read the old v3/v4 metadata tag */
        err = chd_get_metadata(chd, CDROM_TRACK_METADATA_TAG,
                               num_tracks, tmp, sizeof(tmp), NULL, NULL,
                               NULL);
        if (err == CHDERR_NONE) {
          sscanf(tmp, CDROM_TRACK_METADATA_FORMAT, &tkid, type, subtype,
                 &frames);
        } else {
          /* if there's no valid metadata, this is the end of the TOC */
          break;
        }
      }
    
    if (strcmp(type, "MODE1") && strcmp(type, "MODE1_RAW") &&
        strcmp(type, "AUDIO")) {
       log_cb(RETRO_LOG_ERROR, "chd_parse track type %s unsupported\n", type);
      return 0;
    }

    if (strcmp(subtype, "NONE")) {
       log_cb(RETRO_LOG_ERROR, "chd_parse track subtype %s unsupported\n", subtype);
      return 0;
    }

    plba += pregap;

    /* add track */
    num_tracks++;
    tocd.tracks[num_tracks].adr = 1;
    tocd.tracks[num_tracks].control = strcmp(type, "AUDIO") == 0 ? 0 : 4;
    tocd.tracks[num_tracks].lba = plba;
    tocd.tracks[num_tracks].valid = true;

    log_cb(RETRO_LOG_INFO, "chd_parse '%s' track=%d lba=%d\n", tmp, num_tracks,
             plba);

    plba += frames;
    plba += postgap;

    tocd.first_track = 1;
    tocd.last_track = num_tracks;
  }
    img_numsectors = plba;
    log_cb(RETRO_LOG_INFO, "chd img_numsectors '%d'\n", img_numsectors);
  
    /* add track */
    tocd.tracks[100].adr = 1;
    tocd.tracks[100].control = 0;
    tocd.tracks[100].lba = plba;
    tocd.tracks[100].valid = true;
  
#if 0
   {
      CHD_Section& ds = Sections["DISC"];
      unsigned toc_entries = CHD_ReadInt<unsigned>(ds, "TOCENTRIES");
      unsigned num_sessions = CHD_ReadInt<unsigned>(ds, "SESSIONS");
      bool data_tracks_scrambled = CHD_ReadInt<unsigned>(ds, "DATATRACKSSCRAMBLED");

      if(num_sessions != 1)
      {
         log_cb(RETRO_LOG_ERROR, "Unsupported number of sessions: %u\n", num_sessions);
         return false;
      }

      if(data_tracks_scrambled)
      {
         log_cb(RETRO_LOG_ERROR, "Scrambled CHD data tracks currently not supported.\n");
         return false;
      }

      //printf("MOO: %d\n", toc_entries);

      for(unsigned te = 0; te < toc_entries; te++)
      {
         char tmpbuf[64];
         snprintf(tmpbuf, sizeof(tmpbuf), "ENTRY %u", te);
         CHD_Section& ts = Sections[std::string(tmpbuf)];
         unsigned session = CHD_ReadInt<unsigned>(ts, "SESSION");
         uint8_t point = CHD_ReadInt<uint8_t>(ts, "POINT");
         uint8_t adr = CHD_ReadInt<uint8_t>(ts, "ADR");
         uint8_t control = CHD_ReadInt<uint8_t>(ts, "CONTROL");
         uint8_t pmin = CHD_ReadInt<uint8_t>(ts, "PMIN");
         uint8_t psec = CHD_ReadInt<uint8_t>(ts, "PSEC");
         //uint8_t pframe = CHD_ReadInt<uint8_t>(ts, "PFRAME");
         signed plba = CHD_ReadInt<signed>(ts, "PLBA");

         if(session != 1)
         {
            log_cb(RETRO_LOG_ERROR, "Unsupported TOC entry Session value: %u\n", session);
            return false;
         }

         // Reference: ECMA-394, page 5-14
         if(point >= 1 && point <= 99)
         {
            tocd.tracks[point].adr = adr;
            tocd.tracks[point].control = control;
            tocd.tracks[point].lba = plba;
            tocd.tracks[point].valid = true;
         }
         else switch(point)
         {
            default:
               log_cb(RETRO_LOG_ERROR, "Unsupported TOC entry Point value: %u\n", point);
               return false;
            case 0xA0:
               tocd.first_track = pmin;
               tocd.disc_type = psec;
               break;

            case 0xA1:
               tocd.last_track = pmin;
               break;

            case 0xA2:
               tocd.tracks[100].adr = adr;
               tocd.tracks[100].control = control;
               tocd.tracks[100].lba = plba;
               tocd.tracks[100].valid = true;
               break;
         }
      }
   }

#endif

   return true;
}


CDAccess_CHD::~CDAccess_CHD()
{
  if (chd!=NULL)
  chd_close(chd);
}

bool CDAccess_CHD::Read_Raw_Sector(uint8_t *buf, int32_t lba)
{
   if(lba < 0)
   {
      synth_udapp_sector_lba(0xFF, tocd, lba, 0, buf);
      return true; /* TODO/FIXME - see if we need to return false here? */
   }

   if((size_t)lba >= img_numsectors)
   {
      synth_leadout_sector_lba(0xFF, tocd, lba, buf);
      return true; /* TODO/FIXME - see if we need to return false here? */
   }

  const chd_header *head = chd_get_header(chd);
  int cad = lba;// HACK - track->file_offset;
  int hunknum = (cad * head->unitbytes) / head->hunkbytes;
  int hunkofs = (cad * head->unitbytes) % head->hunkbytes;

  /* each hunk holds ~8 sectors, optimize when reading contiguous sectors */
  if (hunknum != oldhunk) {
    int err = chd_read(chd, hunknum, hunkmem);
    if(err!=CHDERR_NONE)
  	log_cb(RETRO_LOG_ERROR, "chd_read_sector failed lba=%d\n", lba);
  }


  memcpy(buf, hunkmem + hunkofs /*+track->header_size*/, 2352 + 96);
  //log_cb(RETRO_LOG_ERROR, "chd_read_sector OK buf=0x%x hunkmem=0x%x hunkofs=%d\n", buf, hunkmem, hunkofs);
  subpw_interleave(hunkmem + hunkofs + 2352, buf + 2352);
  
#if 0
   img_stream->seek(lba * 2352, SEEK_SET);
   img_stream->read(buf, 2352);

   subpw_interleave(&sub_data[lba * 96], buf + 2352);
#endif
   return true;
}

bool CDAccess_CHD::Fast_Read_Raw_PW_TSRE(uint8_t* pwbuf, int32_t lba)
{
  uint8_t buf[2352 + 96];

   if(lba < 0)
   {
      subpw_synth_udapp_lba(tocd, lba, 0, pwbuf);
      return true;
   }

   if((size_t)lba >= img_numsectors)
   {
      subpw_synth_leadout_lba(tocd, lba, pwbuf);
      return true;
   }

    Read_Raw_Sector(buf, lba);
    subpw_interleave(buf + 2352, pwbuf);
    return true;

}

bool CDAccess_CHD::Read_TOC(TOC *toc)
{
   *toc = tocd;
   return true;
}

