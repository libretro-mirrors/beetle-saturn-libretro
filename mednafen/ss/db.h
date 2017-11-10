#ifndef __MDFN_SS_DB_H
#define __MDFN_SS_DB_H

enum
{
 CPUCACHE_EMUMODE_DATA_CB,
 CPUCACHE_EMUMODE_DATA,
 CPUCACHE_EMUMODE_FULL
};

void DB_Lookup(const char* path, const char* sgid, const uint8* fd_id, unsigned* const region, int* const cart_type, unsigned* const cpucache_emumode);

#endif

