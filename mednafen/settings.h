#ifndef MDFN_SETTINGS_H
#define MDFN_SETTINGS_H

#include <string.h>

#include <boolean.h>

#include "mednafen-types.h"

#ifdef __cplusplus
extern "C" {
#endif

extern bool DoHBlend;

// This should assert() or something if the setting isn't found, since it would
// be a totally tubular error!
uint64 MDFN_GetSettingUI(const char *name);
int64 MDFN_GetSettingI(const char *name);
bool MDFN_GetSettingB(const char *name);
const char *MDFN_GetSettingS(const char *name);

#ifdef __cplusplus
}
#endif

#endif
