#ifndef __DISC_H__
#define __DISC_H__

#include "libretro.h"
#include "mednafen/mednafen-types.h"

extern void extract_basename(char *buf, const char *path, size_t size);
extern void extract_directory(char *buf, const char *path, size_t size);

// These routines handle disc drive front-end.

extern unsigned disk_get_image_index(void);

extern void disc_init( retro_environment_t environ_cb );

extern void disc_cleanup();

extern bool disc_detect_region( unsigned* region );

extern bool disc_test();

extern void disc_select( unsigned disc_num );

extern bool disc_load_content( MDFNGI* game_inteface, const char *name, uint8* fd_id, char* sgid, bool image_memcache );

#endif
