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

#include <string>

#include "mednafen.h"
#include "settings.h"
#include <compat/msvc.h>

bool setting_smpc_autortc = true;
int setting_smpc_autortc_lang = 0;
int setting_initial_scanline = 0;
int setting_initial_scanline_pal = 0;
int setting_last_scanline = 239;
int setting_last_scanline_pal = 287;

extern char retro_cd_base_name[4096];
extern char retro_save_directory[4096];
extern char retro_base_directory[4096];

uint64_t MDFN_GetSettingUI(const char *name)
{
   if (!strcmp("ss.scsp.resamp_quality", name)) /* make configurable */
      return 4;
   if (!strcmp("ss.region_default", name))
   {
      /*
       * 0 - jp
       * 1 - na 
       * 2 - eu
       * 3 - kr
       * 4 - tw
       * 5 - as
       * 6 - br
       * 7 - la
       */
      return 0;
   }
   if (!strcmp("ss.smpc.autortc.lang", name))
      return setting_smpc_autortc_lang;
   if (!strcmp("ss.dbg_mask", name))
      return 1;

   fprintf(stderr, "unhandled setting UI: %s\n", name);
   return 0;
}

int64 MDFN_GetSettingI(const char *name)
{
   if (!strcmp("ss.region_default", name))
      return MDFN_GetSettingUI("ss.region_default");
   if (!strcmp("ss.slstart", name))
      return setting_initial_scanline;
   if (!strcmp("ss.slstartp", name))
      return setting_initial_scanline_pal;
   if (!strcmp("ss.slend", name))
      return setting_last_scanline;
   if (!strcmp("ss.slendp", name))
      return setting_last_scanline_pal;
   if (!strcmp("ss.cart", name))
   {
      /* -1 - reserved
       *  0 - auto
       *  1 - none
       *  2 - backup
       *  3 - extram1
       *  4 - extram4
       */
      return -1;
   }
   fprintf(stderr, "unhandled setting I: %s\n", name);
   return 0;
}

double MDFN_GetSettingF(const char *name)
{
   if (!strcmp("ss.input.mouse_sensitivity", name))
      return 0.50; /* TODO - make configurable */

   fprintf(stderr, "unhandled setting F: %s\n", name);
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
      return int(setting_smpc_autortc);
   if (!strcmp("ss.region_autodetect", name)) /* make configurable */
      return 1;
   if (!strcmp("ss.bios_sanity", name))
      return true;
   if (!strcmp("ss.cd_sanity", name))
      return false;
   if (!strcmp("ss.midsync", name))
      return false;
   /* CDROM */
   if (!strcmp("cdrom.lec_eval", name))
      return 1;
   /* FILESYS */
   if (!strcmp("filesys.untrusted_fip_check", name))
      return 0;
   if (!strcmp("filesys.disablesavegz", name))
      return 1;
   fprintf(stderr, "unhandled setting B: %s\n", name);
   return 0;
}

std::string MDFN_GetSettingS(const char *name)
{
   if (!strcmp("ss.bios_jp", name))
      return std::string("sega_101.bin");
   if (!strcmp("ss.bios_na_eu", name))
      return std::string("mpr-17933.bin");
   if (!strcmp("ss.cart.kof95_path", name))
      return std::string("mpr-18811-mx.ic1");
   if (!strcmp("ss.cart.ultraman_path", name))
      return std::string("mpr-19367-mx.ic1");
   if (!strcmp("ss.region_default", name)) /* make configurable */
      return "na";
   /* FILESYS */
   if (!strcmp("filesys.path_firmware", name))
      return std::string(retro_base_directory);
   if (!strcmp("filesys.path_sav", name))
      return std::string(retro_save_directory);
   if (!strcmp("filesys.path_state", name))
      return std::string(retro_save_directory);
   if (!strcmp("filesys.fname_state", name))
   {
      char fullpath[4096];
      snprintf(fullpath, sizeof(fullpath), "%s.sav", retro_cd_base_name);
      return std::string(fullpath);
   }
   if (!strcmp("filesys.fname_sav", name))
   {
      char fullpath[4096];
      snprintf(fullpath, sizeof(fullpath), "%s.bsv", retro_cd_base_name);
      return std::string(fullpath);
   }
   fprintf(stderr, "unhandled setting S: %s\n", name);
   return 0;
}

bool MDFNI_SetSetting(const char *name, const char *value, bool NetplayOverride)
{
   return false;
}

bool MDFNI_SetSettingB(const char *name, bool value)
{
   return false;
}

bool MDFNI_SetSettingUI(const char *name, uint64_t value)
{
   return false;
}
