/* Mednafen - Multi-system Emulator
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <errno.h>
#include <string.h>

#include "settings.h"
#include <compat/msvc.h>
#include "libretro_settings.h"

char retro_cd_base_name[4096];
char retro_save_directory[4096];
char retro_base_directory[4096];

uint64_t MDFN_GetSettingUI(const char *name)
{
   if (!strcmp("ss.scsp.resamp_quality", name)) /* make configurable */
      return 4;
   if (!strcmp("ss.smpc.autortc.lang", name))
      return setting_smpc_autortc_lang;
   if (!strcmp("ss.dbg_mask", name))
      return 1;
   return 0;
}

int64 MDFN_GetSettingI(const char *name)
{
   if (!strcmp("ss.slstart", name))
      return setting_initial_scanline;
   if (!strcmp("ss.slstartp", name))
      return setting_initial_scanline_pal;
   if (!strcmp("ss.slend", name))
      return setting_last_scanline;
   if (!strcmp("ss.slendp", name))
      return setting_last_scanline_pal;
   return 0;
}

bool MDFN_GetSettingB(const char *name)
{
   if (!strcmp("cheats", name))
      return 0;
   /* LIBRETRO */
   if (!strcmp("libretro.cd_load_into_ram", name))
      return 0;
   if (!strcmp("ss.smpc.autortc", name))
      return (int)(setting_smpc_autortc);
   if (!strcmp("ss.bios_sanity", name))
      return true;
   /* CDROM */
   if (!strcmp("cdrom.lec_eval", name))
      return 1;
   /* FILESYS */
   if (!strcmp("filesys.untrusted_fip_check", name))
      return 0;
   return 0;
}

const char *MDFN_GetSettingS(const char *name)
{
   if (!strcmp("ss.cart.kof95_path", name))
      return "mpr-18811-mx.ic1";
   if (!strcmp("ss.cart.ultraman_path", name))
      return "mpr-19367-mx.ic1";
   if (!strcmp("ss.cart.satar4mp_path", name))
      return "satar4mp.bin";
   /* FILESYS */
   if (!strcmp("filesys.path_firmware", name))
      return retro_base_directory;
   if (!strcmp("filesys.path_sav", name))
      return retro_save_directory;
   if (!strcmp("filesys.path_state", name))
      return retro_save_directory;
   return 0;
}
