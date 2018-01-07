#ifndef MDFN_FILE_H
#define MDFN_FILE_H

#include <stdint.h>

#define MDFNFILE_EC_NOTFOUND	1
#define MDFNFILE_EC_OTHER	2

#ifdef __cplusplus
extern "C" {
#endif

struct MDFNFILE
{
   uint8_t *data;
   int64_t size;
   char *ext;
   int64_t location;
};

struct MDFNFILE *file_open(const char *path);

int file_close(struct MDFNFILE *file);

#ifdef __cplusplus
}
#endif

#endif
