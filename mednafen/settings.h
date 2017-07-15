#ifndef MDFN_SETTINGS_H
#define MDFN_SETTINGS_H

#include <string>

extern bool setting_smpc_autortc;
extern int setting_smpc_autortc_lang;
extern int setting_initial_scanline;
extern int setting_initial_scanline_pal;
extern int setting_last_scanline;
extern int setting_last_scanline_pal;

// This should assert() or something if the setting isn't found, since it would
// be a totally tubular error!
uint64 MDFN_GetSettingUI(const char *name);
int64 MDFN_GetSettingI(const char *name);
double MDFN_GetSettingF(const char *name);
bool MDFN_GetSettingB(const char *name);
std::string MDFN_GetSettingS(const char *name);
#endif
