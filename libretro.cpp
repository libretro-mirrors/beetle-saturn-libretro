#include <stdarg.h>
#include "mednafen/mednafen.h"
#include "mednafen/mempatcher.h"
#include "mednafen/git.h"
#include "mednafen/general.h"
#include "mednafen/md5.h"
#include <compat/msvc.h>
#ifdef NEED_DEINTERLACER
#include "mednafen/video/Deinterlacer.h"
#endif
#include "libretro.h"
#include <rthreads/rthreads.h>
#include <retro_stat.h>
#include <string/stdstring.h>
#include "libretro_cbs.h"

struct retro_perf_callback perf_cb;
retro_get_cpu_features_t perf_get_cpu_features_cb = NULL;
retro_log_printf_t log_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_rumble_interface rumble;
static retro_environment_t environ_cb;

static unsigned players = 2;
static unsigned frame_count = 0;
static unsigned internal_frame_count = 0;
static bool failed_init = false;
static unsigned image_offset = 0;
static unsigned image_crop = 0;

// Sets how often (in number of output frames/retro_run invocations)
// the internal framerace counter should be updated if
// display_internal_framerate is true.
#define INTERNAL_FPS_SAMPLE_PERIOD 32

char retro_save_directory[4096];
char retro_base_directory[4096];
static char retro_cd_base_directory[4096];
static char retro_cd_path[4096];
char retro_cd_base_name[4096];
#ifdef _WIN32
   static char retro_slash = '\\';
#else
   static char retro_slash = '/';
#endif

static void extract_basename(char *buf, const char *path, size_t size)
{
   const char *base = strrchr(path, '/');
   if (!base)
      base = strrchr(path, '\\');
   if (!base)
      base = path;

   if (*base == '\\' || *base == '/')
      base++;

   strncpy(buf, base, size - 1);
   buf[size - 1] = '\0';

   char *ext = strrchr(buf, '.');
   if (ext)
      *ext = '\0';
}

static void extract_directory(char *buf, const char *path, size_t size)
{
   strncpy(buf, path, size - 1);
   buf[size - 1] = '\0';

   char *base = strrchr(buf, '/');
   if (!base)
      base = strrchr(buf, '\\');

   if (base)
      *base = '\0';
   else
      buf[0] = '\0';
}

//forward decls
extern void Emulate(EmulateSpecStruct *espec);
extern void SetInput(int port, const char *type, void *ptr);


static bool overscan;
static double last_sound_rate;


#define RETRO_DEVICE_PS1PAD       RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0)
#define RETRO_DEVICE_DUALANALOG   RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 0)
#define RETRO_DEVICE_DUALSHOCK    RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 1)
#define RETRO_DEVICE_FLIGHTSTICK  RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 2)

#ifdef NEED_DEINTERLACER
static bool PrevInterlaced;
static Deinterlacer deint;
#endif

static MDFN_Surface *surf = NULL;

static void alloc_surface() {
  MDFN_PixelFormat pix_fmt(MDFN_COLORSPACE_RGB, 16, 8, 0, 24);
  uint32_t width  = MEDNAFEN_CORE_GEOMETRY_MAX_W;
  uint32_t height = 480;

  if (surf != NULL) {
    delete surf;
  }

  surf = new MDFN_Surface(NULL, width, height, width, pix_fmt);
}

static void check_system_specs(void)
{
   // Hints that we need a fairly powerful system to run this.
   unsigned level = 15;
   environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);
}

static unsigned disk_get_num_images(void)
{
   if(cdifs)
       return cdifs->size();
   return 0;
}

static bool eject_state;
static bool disk_set_eject_state(bool ejected)
{
   log_cb(RETRO_LOG_INFO, "[Mednafen]: Ejected: %u.\n", ejected);
   if (ejected == eject_state)
      return false;

   DoSimpleCommand(ejected ? MDFN_MSC_EJECT_DISK : MDFN_MSC_INSERT_DISK);
   eject_state = ejected;
   return true;
}

static bool disk_get_eject_state(void)
{
   return eject_state;
}

static unsigned disk_get_image_index(void)
{
   // PSX global. Hacky.
   return CD_SelectedDisc;
}

static bool disk_set_image_index(unsigned index)
{
   CD_SelectedDisc = index;
   if (CD_SelectedDisc > disk_get_num_images())
      CD_SelectedDisc = disk_get_num_images();

   // Very hacky. CDSelect command will increment first.
   CD_SelectedDisc--;

   DoSimpleCommand(MDFN_MSC_SELECT_DISK);
   return true;
}

// Mednafen PSX really doesn't support adding disk images on the fly ...
// Hack around this.
static void update_md5_checksum(CDIF *iface)
{
   uint8 LayoutMD5[16];
   md5_context layout_md5;
   CD_TOC toc;

   md5_starts(&layout_md5);

   TOC_Clear(&toc);

   iface->ReadTOC(&toc);

   md5_update_u32_as_lsb(&layout_md5, toc.first_track);
   md5_update_u32_as_lsb(&layout_md5, toc.last_track);
   md5_update_u32_as_lsb(&layout_md5, toc.tracks[100].lba);

   for (uint32 track = toc.first_track; track <= toc.last_track; track++)
   {
      md5_update_u32_as_lsb(&layout_md5, toc.tracks[track].lba);
      md5_update_u32_as_lsb(&layout_md5, toc.tracks[track].control & 0x4);
   }

   md5_finish(&layout_md5, LayoutMD5);
   memcpy(MDFNGameInfo->MD5, LayoutMD5, 16);

   char *md5 = md5_asciistr(MDFNGameInfo->MD5);
   log_cb(RETRO_LOG_INFO, "[Mednafen]: Updated md5 checksum: %s.\n", md5);
}

// Untested ...
static bool disk_replace_image_index(unsigned index, const struct retro_game_info *info)
{
#if 0
   if (index >= disk_get_num_images() || !eject_state)
      return false;

   if (!info)
   {
      delete cdifs->at(index);
      cdifs->erase(cdifs->begin() + index);
      if (index < CD_SelectedDisc)
         CD_SelectedDisc--;

      // Poke into psx.cpp
      CalcDiscSCEx();
      return true;
   }

   bool success = true;
   CDIF *iface = CDIF_Open(&success, info->path, false, false);

   if (!success)
      return false;

   delete cdifs->at(index);
   cdifs->at(index) = iface;
   CalcDiscSCEx();

   /* If we replace, we want the "swap disk manually effect". */
   extract_basename(retro_cd_base_name, info->path, sizeof(retro_cd_base_name));
   /* Ugly, but needed to get proper disk swapping effect. */
   update_md5_checksum(iface);
   return true;
#else
   return false;
#endif
}

static bool disk_add_image_index(void)
{
   cdifs->push_back(NULL);
   return true;
}

static struct retro_disk_control_callback disk_interface = {
   disk_set_eject_state,
   disk_get_eject_state,
   disk_get_image_index,
   disk_set_image_index,
   disk_get_num_images,
   disk_replace_image_index,
   disk_add_image_index,
};

static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
}


void retro_init(void)
{
   struct retro_log_callback log;

   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = fallback_log;

#ifdef NEED_CD
   CDUtility_Init();
#endif

   eject_state = false;

   const char *dir = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
   {
      snprintf(retro_base_directory, sizeof(retro_base_directory), "%s", dir);
   }
   else
   {
      /* TODO: Add proper fallback */
      log_cb(RETRO_LOG_WARN, "System directory is not defined. Fallback on using same dir as ROM for system directory later ...\n");
      failed_init = true;
   }

   if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir)
   {
      // If save directory is defined use it, otherwise use system directory
      if (dir)
         snprintf(retro_save_directory, sizeof(retro_save_directory), "%s", dir);
      else
         snprintf(retro_save_directory, sizeof(retro_save_directory), "%s", retro_base_directory);
   }
   else
   {
      /* TODO: Add proper fallback */
      log_cb(RETRO_LOG_WARN, "Save directory is not defined. Fallback on using SYSTEM directory ...\n");
      snprintf(retro_save_directory, sizeof(retro_save_directory), "%s", retro_base_directory);
   }

   environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE, &disk_interface);

   if (environ_cb(RETRO_ENVIRONMENT_GET_PERF_INTERFACE, &perf_cb))
      perf_get_cpu_features_cb = perf_cb.get_cpu_features;
   else
      perf_get_cpu_features_cb = NULL;

   setting_initial_scanline = 0;
   setting_last_scanline = 239;
   setting_initial_scanline_pal = 0;
   setting_last_scanline_pal = 287;

   check_system_specs();
}

void retro_reset(void)
{
   DoSimpleCommand(MDFN_MSC_RESET);
}

bool retro_load_game_special(unsigned, const struct retro_game_info *, size_t)
{
   return false;
}

static bool old_cdimagecache = false;

static bool boot = true;

// shared memory cards support
static bool shared_memorycards = false;
static bool shared_memorycards_toggle = false;

static void check_variables(bool startup)
{
   struct retro_variable var = {0};

   extern void PSXDitherApply(bool);

   if (startup)
   {
   }

   var.key = "beetle_psx_cdimagecache";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      bool cdimage_cache = true;
      if (strcmp(var.value, "enabled") == 0)
         cdimage_cache = true;
      else if (strcmp(var.value, "disabled") == 0)
         cdimage_cache = false;
      if (cdimage_cache != old_cdimagecache)
      {
         old_cdimagecache = cdimage_cache;
      }
   }

   var.key = "beetle_psx_initial_scanline";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      setting_initial_scanline = atoi(var.value);
   }

   var.key = "beetle_psx_last_scanline";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      setting_last_scanline = atoi(var.value);
   }

   var.key = "beetle_psx_initial_scanline_pal";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      setting_initial_scanline_pal = atoi(var.value);
   }

   var.key = "beetle_psx_last_scanline_pal";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      setting_last_scanline_pal = atoi(var.value);
   }

   if(setting_psx_multitap_port_1)
   {
      if(setting_psx_multitap_port_2)
         players = 8;
      else
         players = 4;
   }
   else
   {
      if(setting_psx_multitap_port_2)
         players = 4;
      else
         players = 2;
   }
}

#ifdef NEED_CD
static void ReadM3U(std::vector<std::string> &file_list, std::string path, unsigned depth = 0)
{
   std::string dir_path;
   char linebuf[2048];
   FILE *fp = fopen(path.c_str(), "rb");

   if (fp == NULL)
      return;

   MDFN_GetFilePathComponents(path, &dir_path);

   while(fgets(linebuf, sizeof(linebuf), fp) != NULL)
   {
      std::string efp;

      if(linebuf[0] == '#')
         continue;
      string_trim_whitespace_right(linebuf);
      if(linebuf[0] == 0)
         continue;

      efp = MDFN_EvalFIP(dir_path, std::string(linebuf));

      if(efp.size() >= 4 && efp.substr(efp.size() - 4) == ".m3u")
      {
         if(efp == path)
         {
            log_cb(RETRO_LOG_ERROR, "M3U at \"%s\" references self.\n", efp.c_str());
            goto end;
         }

         if(depth == 99)
         {
            log_cb(RETRO_LOG_ERROR, "M3U load recursion too deep!\n");
            goto end;
         }

         ReadM3U(file_list, efp, depth++);
      }
      else
         file_list.push_back(efp);
   }

end:
   fclose(fp);
}

#ifdef NEED_CD
static std::vector<CDIF *> CDInterfaces;	// FIXME: Cleanup on error out.
#endif
// TODO: LoadCommon()

static MDFNGI *MDFNI_LoadCD(const char *force_module, const char *devicename)
{
   uint8 LayoutMD5[16];

   log_cb(RETRO_LOG_INFO, "Loading %s...\n", devicename);

   try
   {
      if(devicename && strlen(devicename) > 4 && !strcasecmp(devicename + strlen(devicename) - 4, ".m3u"))
      {
         std::vector<std::string> file_list;

         ReadM3U(file_list, devicename);

         for(unsigned i = 0; i < file_list.size(); i++)
         {
            bool success = true;
            CDIF *image  = CDIF_Open(&success, file_list[i].c_str(), false, old_cdimagecache);
            CDInterfaces.push_back(image);
         }
      }
      else if(devicename && strlen(devicename) > 4 && !strcasecmp(devicename + strlen(devicename) - 4, ".pbp"))
      {
         bool success = true;
         CDIF *image  = CDIF_Open(&success, devicename, false, old_cdimagecache);
         CDInterfaces.push_back(image);
      }
      else
      {
         bool success = true;
         CDIF *image  = CDIF_Open(&success, devicename, false, old_cdimagecache);
         CDInterfaces.push_back(image);
      }
   }
   catch(std::exception &e)
   {
      log_cb(RETRO_LOG_ERROR, "Error opening CD.\n");
      return(0);
   }

   // Print out a track list for all discs.
   for(unsigned i = 0; i < CDInterfaces.size(); i++)
   {
      TOC toc;
      TOC_Clear(&toc);

      CDInterfaces[i]->ReadTOC(&toc);

      log_cb(RETRO_LOG_DEBUG, "CD %d Layout:\n", i + 1);

      for(int32 track = toc.first_track; track <= toc.last_track; track++)
      {
         log_cb(RETRO_LOG_DEBUG, "Track %2d, LBA: %6d  %s\n", track, toc.tracks[track].lba, (toc.tracks[track].control & 0x4) ? "DATA" : "AUDIO");
      }

      log_cb(RETRO_LOG_DEBUG, "Leadout: %6d\n", toc.tracks[100].lba);
   }

   // Calculate layout MD5.  The system emulation LoadCD() code is free to ignore this value and calculate
   // its own, or to use it to look up a game in its database.
   {
      md5_context layout_md5;

      md5_starts(&layout_md5);

      for(unsigned i = 0; i < CDInterfaces.size(); i++)
      {
         CD_TOC toc;

         TOC_Clear(&toc);
         CDInterfaces[i]->ReadTOC(&toc);

         md5_update_u32_as_lsb(&layout_md5, toc.first_track);
         md5_update_u32_as_lsb(&layout_md5, toc.last_track);
         md5_update_u32_as_lsb(&layout_md5, toc.tracks[100].lba);

         for(uint32 track = toc.first_track; track <= toc.last_track; track++)
         {
            md5_update_u32_as_lsb(&layout_md5, toc.tracks[track].lba);
            md5_update_u32_as_lsb(&layout_md5, toc.tracks[track].control & 0x4);
         }
      }

      md5_finish(&layout_md5, LayoutMD5);
   }

   // TODO: include module name in hash
   memcpy(MDFNGameInfo->MD5, LayoutMD5, 16);

   if(!(LoadCD(&CDInterfaces)))
   {
      for(unsigned i = 0; i < CDInterfaces.size(); i++)
         delete CDInterfaces[i];
      CDInterfaces.clear();

      MDFNGameInfo = NULL;
      return(0);
   }

   //MDFNI_SetLayerEnableMask(~0ULL);

   MDFN_LoadGameCheats(NULL);
   MDFNMP_InstallReadPatches();

   return(MDFNGameInfo);
}
#endif

static MDFNGI *MDFNI_LoadGame(const char *force_module, const char *name)
{
   MDFNFILE *GameFile;

#ifdef NEED_CD
	if(strlen(name) > 4 && (!strcasecmp(name + strlen(name) - 4, ".cue") || !strcasecmp(name + strlen(name) - 4, ".ccd") || !strcasecmp(name + strlen(name) - 4, ".toc") || !strcasecmp(name + strlen(name) - 4, ".m3u") || !strcasecmp(name + strlen(name) - 4, ".pbp")))
	 return MDFNI_LoadCD(force_module, name);
#endif

   GameFile = file_open(name);

   if(!GameFile)
      goto error;

   if(Load(name, GameFile) <= 0)
      goto error;

   file_close(GameFile);
   GameFile   = NULL;

   return(MDFNGameInfo);

error:
   if (GameFile)
      file_close(GameFile);
   GameFile     = NULL;
   MDFNGameInfo = NULL;
   return NULL;
}

#define MAX_PLAYERS 8
#define MAX_BUTTONS 16

union
{
   uint32_t u32[MAX_PLAYERS][1 + 8 + 1]; // Buttons + Axes + Rumble
   uint8_t u8[MAX_PLAYERS][(1 + 8 + 1) * sizeof(uint32_t)];
} static buf;

static uint16_t input_buf[MAX_PLAYERS] = {0};

bool retro_load_game(const struct retro_game_info *info)
{
   bool ret = false;

   char tocbasepath[4096];
   if (failed_init)
      return false;

   struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },


      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 1, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },

      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 2, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 2, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 2, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 2, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },

      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 3, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 3, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 3, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 3, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },

      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 4, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 4, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 4, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 4, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 4, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },

      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 5, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 5, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 5, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 5, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 5, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },

      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 6, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 6, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 6, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 6, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 6, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },

      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Cross" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Circle" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Triangle" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Square" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "L1" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,    "L2" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,    "L3" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "R1" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,    "R2" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,    "R3" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Select" },
      { 7, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },
      { 7, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Analog X" },
      { 7, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Analog Y" },
      { 7, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Analog X" },
      { 7, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Analog Y" },

      { 0 },
   };

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
      return false;

   extract_basename(retro_cd_base_name,       info->path, sizeof(retro_cd_base_name));
   extract_directory(retro_cd_base_directory, info->path, sizeof(retro_cd_base_directory));

   snprintf(tocbasepath, sizeof(tocbasepath), "%s%c%s.toc", retro_cd_base_directory, retro_slash, retro_cd_base_name);

   if (path_is_valid(tocbasepath))
      snprintf(retro_cd_path, sizeof(retro_cd_path), "%s", tocbasepath);
   else
      snprintf(retro_cd_path, sizeof(retro_cd_path), "%s", info->path);

   check_variables(true);
   //make sure shared memory cards and save states are enabled only at startup
   shared_memorycards = shared_memorycards_toggle;

   if (environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble) && log_cb)
      log_cb(RETRO_LOG_INFO, "Rumble interface supported!\n");

   if (!MDFNI_LoadGame(MEDNAFEN_CORE_NAME_MODULE, retro_cd_path))
   {
      failed_init = true;
      return false;
   }

   MDFN_LoadGameCheats(NULL);
   MDFNMP_InstallReadPatches();

   alloc_surface();

#ifdef NEED_DEINTERLACER
   PrevInterlaced = false;
   deint.ClearState();
#endif

   //SetInput(0, "gamepad", &input_buf[0]);
   //SetInput(1, "gamepad", &input_buf[1]);

   for (unsigned i = 0; i < players; i++)
   {
      SetInput(i, "gamepad", &input_buf[i]);
   }
   boot = false;

   frame_count = 0;
   internal_frame_count = 0;

   return true;
}

void retro_unload_game(void)
{
   if(!MDFNGameInfo)
      return;

   MDFN_FlushGameCheats(0);

   CloseGame();

   MDFNMP_Kill();

   MDFNGameInfo = NULL;

#ifdef NEED_CD
   for(unsigned i = 0; i < CDInterfaces.size(); i++)
      delete CDInterfaces[i];
   CDInterfaces.clear();
#endif

   retro_cd_base_directory[0] = '\0';
   retro_cd_path[0]           = '\0';
   retro_cd_base_name[0]      = '\0';
}


// Hardcoded for PSX. No reason to parse lots of structures ...
// See mednafen/psx/input/gamepad.cpp
static void update_input(void)
{
   //input_buf[0] = 0;
   //input_buf[1] = 0;

   for (unsigned j = 0; j < players; j++)
   {
       input_buf[j] = 0;
   }

   static unsigned map[] = {
      RETRO_DEVICE_ID_JOYPAD_SELECT,
      RETRO_DEVICE_ID_JOYPAD_L3,
      RETRO_DEVICE_ID_JOYPAD_R3,
      RETRO_DEVICE_ID_JOYPAD_START,
      RETRO_DEVICE_ID_JOYPAD_UP,
      RETRO_DEVICE_ID_JOYPAD_RIGHT,
      RETRO_DEVICE_ID_JOYPAD_DOWN,
      RETRO_DEVICE_ID_JOYPAD_LEFT,
      RETRO_DEVICE_ID_JOYPAD_L2,
      RETRO_DEVICE_ID_JOYPAD_R2,
      RETRO_DEVICE_ID_JOYPAD_L,
      RETRO_DEVICE_ID_JOYPAD_R,
      RETRO_DEVICE_ID_JOYPAD_X,
      RETRO_DEVICE_ID_JOYPAD_A,
      RETRO_DEVICE_ID_JOYPAD_B,
      RETRO_DEVICE_ID_JOYPAD_Y,
   };

   for (unsigned j = 0; j < players; j++)
   {
      for (unsigned i = 0; i < MAX_BUTTONS; i++)
         input_buf[j] |= input_state_cb(j, RETRO_DEVICE_JOYPAD, 0, map[i]) ? (1 << i) : 0;
   }

   // Buttons.
   //buf.u8[0][0] = (input_buf[0] >> 0) & 0xff;
   //buf.u8[0][1] = (input_buf[0] >> 8) & 0xff;
   //buf.u8[1][0] = (input_buf[1] >> 0) & 0xff;
   //buf.u8[1][1] = (input_buf[1] >> 8) & 0xff;

   for (unsigned j = 0; j < players; j++)
   {
        buf.u8[j][0] = (input_buf[j] >> 0) & 0xff;
        buf.u8[j][1] = (input_buf[j] >> 8) & 0xff;
   }

   // Analogs
   for (unsigned j = 0; j < players; j++)
   {
      int analog_left_x = input_state_cb(j, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
            RETRO_DEVICE_ID_ANALOG_X);

      int analog_left_y = input_state_cb(j, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
            RETRO_DEVICE_ID_ANALOG_Y);

      int analog_right_x = input_state_cb(j, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
            RETRO_DEVICE_ID_ANALOG_X);

      int analog_right_y = input_state_cb(j, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
            RETRO_DEVICE_ID_ANALOG_Y);

      uint32_t r_right = analog_right_x > 0 ?  analog_right_x : 0;
      uint32_t r_left  = analog_right_x < 0 ? -analog_right_x : 0;
      uint32_t r_down  = analog_right_y > 0 ?  analog_right_y : 0;
      uint32_t r_up    = analog_right_y < 0 ? -analog_right_y : 0;

      uint32_t l_right = analog_left_x > 0 ?  analog_left_x : 0;
      uint32_t l_left  = analog_left_x < 0 ? -analog_left_x : 0;
      uint32_t l_down  = analog_left_y > 0 ?  analog_left_y : 0;
      uint32_t l_up    = analog_left_y < 0 ? -analog_left_y : 0;

      buf.u32[j][1] = r_right;
      buf.u32[j][2] = r_left;
      buf.u32[j][3] = r_down;
      buf.u32[j][4] = r_up;

      buf.u32[j][5] = l_right;
      buf.u32[j][6] = l_left;
      buf.u32[j][7] = l_down;
      buf.u32[j][8] = l_up;
   }

   //fprintf(stderr, "Rumble strong: %u, weak: %u.\n", buf.u8[0][9 * 4 + 1], buf.u8[0][9 * 4]);
   if (rumble.set_rumble_state)
   {
      // Appears to be correct.
      //rumble.set_rumble_state(0, RETRO_RUMBLE_WEAK, buf.u8[0][9 * 4] * 0x101);
      //rumble.set_rumble_state(0, RETRO_RUMBLE_STRONG, buf.u8[0][9 * 4 + 1] * 0x101);
      //rumble.set_rumble_state(1, RETRO_RUMBLE_WEAK, buf.u8[1][9 * 4] * 0x101);
      //rumble.set_rumble_state(1, RETRO_RUMBLE_STRONG, buf.u8[1][9 * 4 + 1] * 0x101);

      for (unsigned j = 0; j < players; j++)
      {
          rumble.set_rumble_state(j, RETRO_RUMBLE_WEAK, buf.u8[j][9 * 4] * 0x101);
          rumble.set_rumble_state(j, RETRO_RUMBLE_STRONG, buf.u8[j][9 * 4 + 1] * 0x101);
      }
   }
}

static uint64_t video_frames, audio_frames;
#define SOUND_CHANNELS 2

void retro_run(void)
{
   bool updated = false;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
   {
      check_variables(false);
      struct retro_system_av_info new_av_info;
      retro_get_system_av_info(&new_av_info);
      environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &new_av_info);
   }

   // Keep the counters at 0 so that they don't display a bogus
   // value if this option is enabled later on
   frame_count = 0;
   internal_frame_count = 0;

   input_poll_cb();

   update_input();

   static int32 rects[MEDNAFEN_CORE_GEOMETRY_MAX_H];
   rects[0] = ~0;

   EmulateSpecStruct spec = {0};
   spec.surface = surf;
   spec.SoundRate = 44100;
   spec.SoundBuf = NULL;
   spec.LineWidths = rects;
   spec.SoundBufMaxSize = 0;
   spec.SoundVolume = 1.0;
   spec.soundmultiplier = 1.0;
   spec.SoundBufSize = 0;
   spec.VideoFormatChanged = false;
   spec.SoundFormatChanged = false;

   EmulateSpecStruct *espec = (EmulateSpecStruct*)&spec;

   Emulate(espec);

   const void *fb        = NULL;
   unsigned width        = rects[0];
   unsigned height       = spec.DisplayRect.h;

   int16_t *interbuf = (int16_t*)&IntermediateBuffer;

   video_frames++;
   audio_frames += spec.SoundBufSize;

   audio_batch_cb(interbuf, spec.SoundBufSize);
}

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = MEDNAFEN_CORE_NAME;
   info->library_version  = MEDNAFEN_CORE_VERSION;
   info->need_fullpath    = true;
   info->valid_extensions = MEDNAFEN_CORE_EXTENSIONS;
   info->block_extract    = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));
   info->timing.fps            = MEDNAFEN_CORE_TIMING_FPS;
   info->timing.sample_rate    = 44100;
   info->geometry.base_width   = MEDNAFEN_CORE_GEOMETRY_BASE_W;
   info->geometry.base_height  = MEDNAFEN_CORE_GEOMETRY_BASE_H;
   info->geometry.max_width    = MEDNAFEN_CORE_GEOMETRY_MAX_W;
   info->geometry.max_height   = MEDNAFEN_CORE_GEOMETRY_MAX_H;
   info->geometry.aspect_ratio = MEDNAFEN_CORE_GEOMETRY_ASPECT_RATIO;
}

void retro_deinit(void)
{
   delete surf;
   surf = NULL;

   log_cb(RETRO_LOG_INFO, "[%s]: Samples / Frame: %.5f\n",
         MEDNAFEN_CORE_NAME, (double)audio_frames / video_frames);
   log_cb(RETRO_LOG_INFO, "[%s]: Estimated FPS: %.5f\n",
         MEDNAFEN_CORE_NAME, (double)video_frames * 44100 / audio_frames);
}

unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void retro_set_controller_port_device(unsigned in_port, unsigned device)
{
   switch (device)
   {
      case RETRO_DEVICE_JOYPAD:
      case RETRO_DEVICE_PS1PAD:
         log_cb(RETRO_LOG_INFO, "[%s]: Selected controller type standard gamepad.\n", MEDNAFEN_CORE_NAME);
         SetInput(in_port, "gamepad", &buf.u8[in_port]);
         break;
      case RETRO_DEVICE_DUALANALOG:
         log_cb(RETRO_LOG_INFO, "[%s]: Selected controller type Dual Analog.\n", MEDNAFEN_CORE_NAME);
         SetInput(in_port, "dualanalog", &buf.u8[in_port]);
         break;
      case RETRO_DEVICE_DUALSHOCK:
         log_cb(RETRO_LOG_INFO, "[%s]: Selected controller type DualShock.\n", MEDNAFEN_CORE_NAME);
         SetInput(in_port, "dualshock", &buf.u8[in_port]);
         break;
      case RETRO_DEVICE_FLIGHTSTICK:
         log_cb(RETRO_LOG_INFO, "[%s]: Selected controller type FlightStick.\n", MEDNAFEN_CORE_NAME);
         SetInput(in_port, "analogjoy", &buf.u8[in_port]);
         break;
      default:
         log_cb(RETRO_LOG_WARN, "[%s]: Unsupported controller device %u, falling back to gamepad.\n", MEDNAFEN_CORE_NAME,device);
   }

   if (rumble.set_rumble_state)
   {
      rumble.set_rumble_state(in_port, RETRO_RUMBLE_STRONG, 0);
      rumble.set_rumble_state(in_port, RETRO_RUMBLE_WEAK, 0);
      buf.u32[in_port][9] = 0;
   }
}

#if defined(HAVE_OPENGL)
#define FIRST_RENDERER "opengl"
#define EXT_RENDERER "|software"
#elif defined(HAVE_RUST)
#define FIRST_RENDERER "opengl-rust"
#define EXT_RENDERER "|software"
#else
#define FIRST_RENDERER "software"
#define EXT_RENDERER ""
#endif

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;

   static const struct retro_variable vars[] = {
      { "beetle_psx_cdimagecache", "CD Image Cache (restart); disabled|enabled" },
      { "beetle_psx_initial_scanline", "Initial scanline; 0|1|2|3|4|5|6|7|8|9|10|10|11|12|13|14|15|16|17|18|19|20|21|22|23|24|25|26|27|28|29|30|31|32|33|34|35|36|37|38|39|40" },
      { "beetle_psx_initial_scanline_pal", "Initial scanline PAL; 0|1|2|3|4|5|6|7|8|9|10|10|11|12|13|14|15|16|17|18|19|20|21|22|23|24|25|26|27|28|29|30|31|32|33|34|35|36|37|38|39|40" },
      { "beetle_psx_last_scanline", "Last scanline; 239|238|237|236|235|234|232|231|230|229|228|227|226|225|224|223|222|221|220|219|218|217|216|215|214|213|212|211|210" },
      { "beetle_psx_last_scanline_pal", "Last scanline PAL; 287|286|285|284|283|283|282|281|280|279|278|277|276|275|274|273|272|271|270|269|268|267|266|265|264|263|262|261|260" },
      { NULL, NULL },
   };
   static const struct retro_controller_description pads[] = {
      { "PS1 Joypad", RETRO_DEVICE_JOYPAD },
      { "DualAnalog", RETRO_DEVICE_DUALANALOG },
      { "DualShock", RETRO_DEVICE_DUALSHOCK },
      { "FlightStick", RETRO_DEVICE_FLIGHTSTICK },
   };

   static const struct retro_controller_info ports[] = {
      { pads, 4 },
      { pads, 4 },
      { pads, 4 },
      { pads, 4 },
      { pads, 4 },
      { pads, 4 },
      { pads, 4 },
      { pads, 4 },
      { 0 },
   };

   cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
   environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_state_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

static size_t serialize_size;

size_t retro_serialize_size(void)
{
   StateMem st;
   memset(&st, 0, sizeof(st));

   if (!MDFNSS_SaveSM(&st, 0, 0, NULL, NULL, NULL))
   {
      return 0;
   }

   free(st.data);
   return serialize_size = st.len;
}

bool retro_serialize(void *data, size_t size)
{
   /* it seems that mednafen can realloc pointers sent to it?
      since we don't know the disposition of void* data (is it safe to realloc?) we have to manage a new buffer here */
   StateMem st;
   memset(&st, 0, sizeof(st));
   st.data     = (uint8_t*)malloc(size);
   st.malloced = size;

   bool ret = MDFNSS_SaveSM(&st, 0, 0, NULL, NULL, NULL);

   /* there are still some errors with the save states, the size seems to change on some games for now just log when this happens */
   if (st.len != size)
      log_cb(RETRO_LOG_WARN, "warning, save state size has changed\n");

   memcpy(data,st.data,size);
   free(st.data);
return ret;

}
bool retro_unserialize(const void *data, size_t size)
{
   StateMem st;
   memset(&st, 0, sizeof(st));
   st.data = (uint8_t*)data;
   st.len  = size;

   return MDFNSS_LoadSM(&st, 0, 0);
}

void *retro_get_memory_data(unsigned type)
{
   return NULL;
}

size_t retro_get_memory_size(unsigned type)
{
   return 0;
}

void retro_cheat_reset(void)
{}

void retro_cheat_set(unsigned, bool, const char *)
{}

#ifdef _WIN32
static void sanitize_path(std::string &path)
{
   size_t size = path.size();
   for (size_t i = 0; i < size; i++)
      if (path[i] == '/')
         path[i] = '\\';
}
#endif

// Use a simpler approach to make sure that things go right for libretro.
const char *MDFN_MakeFName(MakeFName_Type type, int id1, const char *cd1)
{
   static char fullpath[4096];

   fullpath[0] = '\0';

   switch (type)
   {
      case MDFNMKF_SAV:
         snprintf(fullpath, sizeof(fullpath), "%s%c%s.%s",
               retro_save_directory,
               retro_slash,
               (!shared_memorycards) ? retro_cd_base_name : "mednafen_psx_libretro_shared",
               cd1);
         break;
      case MDFNMKF_FIRMWARE:
         snprintf(fullpath, sizeof(fullpath), "%s%c%s", retro_base_directory, retro_slash, cd1);
         break;
      default:
         break;
   }

   return fullpath;
}

void MDFND_DispMessage(unsigned char *str)
{
   const char *strc = (const char*)str;
   struct retro_message msg =
   {
      strc,
      180
   };

   environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
}

void MDFN_DispMessage(const char *format, ...)
{
   char *str = new char[4096];
   struct retro_message msg;
   va_list ap;
   va_start(ap,format);
   const char *strc = NULL;

   vsnprintf(str, 4096, format, ap);
   va_end(ap);
   strc = str;

   msg.frames = 180;
   msg.msg = strc;

   environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
}
