#include <boolean.h>
#include "libretro.h"

bool content_is_pal = false;
retro_video_refresh_t video_cb = NULL;
retro_environment_t environ_cb = NULL;
uint8_t widescreen_hack = 0;
uint8_t psx_gpu_upscale_shift = 0;

float video_output_framerate(void)
{
   return content_is_pal ? 49.842 : 59.941;
}
