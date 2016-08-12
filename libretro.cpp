#include <stdarg.h>
#include "mednafen/mednafen.h"
#include "mednafen/mempatcher.h"
#include "mednafen/git.h"
#include "mednafen/general.h"
#include <compat/msvc.h>
#ifdef NEED_DEINTERLACER
#include "mednafen/video/Deinterlacer.h"
#endif
#include "libretro.h"
#include <rthreads/rthreads.h>
#include <retro_stat.h>
#include <string/stdstring.h>
#include "libretro_cbs.h"

#include <mednafen/mednafen.h>
#include <mednafen/cdrom/cdromif.h>
#include <mednafen/general.h>
#include <mednafen/FileStream.h>
#include <mednafen/mempatcher.h>
#include <mednafen/hash/sha256.h>
#include "mednafen/hash/md5.h"

#include <ctype.h>
#include <time.h>

#include <bitset>

#include <zlib.h>

#define MEDNAFEN_CORE_NAME_MODULE "ss"
#define MEDNAFEN_CORE_NAME "Mednafen Saturn"
#define MEDNAFEN_CORE_VERSION "v0.9.38.7"
#define MEDNAFEN_CORE_EXTENSIONS "pce|cue|ccd"
#define MEDNAFEN_CORE_TIMING_FPS 59.82
#define MEDNAFEN_CORE_GEOMETRY_BASE_W 320
#define MEDNAFEN_CORE_GEOMETRY_BASE_H 240
#define MEDNAFEN_CORE_GEOMETRY_MAX_W 704
#define MEDNAFEN_CORE_GEOMETRY_MAX_H 512
#define MEDNAFEN_CORE_GEOMETRY_ASPECT_RATIO (4.0 / 3.0)

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

extern MDFNGI EmulatedSS;
MDFNGI *MDFNGameInfo = &EmulatedSS;

#include "../MemoryStream.h"

#include "mednafen/ss/ss.h"
#include "mednafen/ss/sound.h"
#include "mednafen/ss/smpc.h"
#include "mednafen/ss/cdb.h"
#include "mednafen/ss/vdp1.h"
#include "mednafen/ss/vdp2.h"
#include "mednafen/ss/scu.h"
#include "mednafen/ss/cart.h"
#include "mednafen/ss/db.h"

void MDFN_BackupSavFile(const uint8 max_backup_count, const char* sav_ext)
{
#if 0
 FileStream cts(MDFN_MakeFName(MDFNMKF_SAVBACK, -1, sav_ext), MODE_READ_WRITE, true);
 std::unique_ptr<MemoryStream> tmp;
 uint8 counter = max_backup_count - 1;

 cts.read(&counter, 1, false);

 tmp.reset(new MemoryStream(new FileStream(MDFN_MakeFName(MDFNMKF_SAV, 0, sav_ext), MODE_READ)));
 MemoryStream oldbks(new FileStream(MDFN_MakeFName(MDFNMKF_SAVBACK, counter, sav_ext), MODE_READ));

 if(oldbks.size() == tmp->size() && !memcmp(oldbks.map(), tmp->map(), oldbks.size()))
 {
    //puts("Skipped backup.");
    return;
 }

 counter = (counter + 1) % max_backup_count;
 FileStream bks(MDFN_MakeFName(MDFNMKF_SAVBACK, counter, sav_ext), MODE_WRITE, 9);

 bks.write(tmp->map(), tmp->size());

 bks.close();

 //
 //
 cts.rewind();
 cts.write(&counter, 1);
 cts.close();
#endif
}

static sscpu_timestamp_t MidSync(const sscpu_timestamp_t timestamp);

uint32 ss_dbg_mask;
static const uint8 BRAM_Init_Data[0x10] = { 0x42, 0x61, 0x63, 0x6b, 0x55, 0x70, 0x52, 0x61, 0x6d, 0x20, 0x46, 0x6f, 0x72, 0x6d, 0x61, 0x74 };

static void SaveBackupRAM(void);
static void LoadBackupRAM(void);
static void SaveCartNV(void);
static void LoadCartNV(void);
static void SaveRTC(void);
static void LoadRTC(void);

static MDFN_COLD void BackupBackupRAM(void);
static MDFN_COLD void BackupCartNV(void);


#include "mednafen/ss/sh7095.h"

static uint8 SCU_MSH2VectorFetch(void);
static uint8 SCU_SSH2VectorFetch(void);

SH7095 CPU[2]{ {"SH2-M", SS_EVENT_SH2_M_DMA, SCU_MSH2VectorFetch}, {"SH2-S", SS_EVENT_SH2_S_DMA, SCU_SSH2VectorFetch}};
static uint16 BIOSROM[524288 / sizeof(uint16)];
static uint16 WorkRAML[1024 * 1024 / sizeof(uint16)];
static uint16 WorkRAMH[1024 * 1024 / sizeof(uint16)];	// Effectively 32-bit in reality, but 16-bit here because of CPU interpreter design(regarding fastmap).
static uint8 BackupRAM[32768];
static bool BackupRAM_Dirty;
static int64 BackupRAM_SaveDelay;
static int64 CartNV_SaveDelay;

#define SH7095_EXT_MAP_GRAN_BITS 16
static uintptr_t SH7095_FastMap[1U << (32 - SH7095_EXT_MAP_GRAN_BITS)];

int32 SH7095_mem_timestamp;
uint32 SH7095_BusLock;

#include "mednafen/ss/scu.inc"
#include "mednafen/ss/debug.inc"

static sha256_digest BIOS_SHA256;	// SHA-256 hash of the currently-loaded BIOS; used for save state sanity checks.
static std::vector<CDIF*> *cdifs = NULL;
static std::bitset<1U << (27 - SH7095_EXT_MAP_GRAN_BITS)> FMIsWriteable;

template<typename T>
static void INLINE SH7095_BusWrite(uint32 A, T V, const bool BurstHax, int32* SH2DMAHax);

template<typename T>
static T INLINE SH7095_BusRead(uint32 A, const bool BurstHax, int32* SH2DMAHax);

// SH-2 region 
//  0: 0x00000000-0x01FFFFFF
//  1: 0x02000000-0x03FFFFFF
//  2: 0x04000000-0x05FFFFFF
//  3: 0x06000000-0x07FFFFFF
//
// Never add anything to SH7095_mem_timestamp when DMAHax is true.
//
// When BurstHax is true and we're accessing high work RAM, don't add anything.
//
template<typename T, bool IsWrite>
static INLINE void BusRW(uint32 A, T& V, const bool BurstHax, int32* SH2DMAHax)
{
 //
 // High work RAM
 //
 if(A >= 0x06000000 && A <= 0x07FFFFFF)
 {
  ne16_rwbo_be<T, IsWrite>(WorkRAMH, A & 0xFFFFF, &V);

  if(!BurstHax)
  {
   if(!SH2DMAHax)
    SH7095_mem_timestamp += IsWrite ? 4 : 7;
   else
    *SH2DMAHax -= IsWrite ? 3 : 6;
  }

  return;
 }

 //
 //
 // SH-2 region 0
 //
 //  Note: 0x00400000 - 0x01FFFFFF: Open bus for accesses to 0x00000000-0x01FFFFFF(SH-2 area 0)
 //
 if(A < 0x02000000)
 {
  if(sizeof(T) == 4)
  {
   if(IsWrite)
   {
    uint16 tmp;

    tmp = V >> 16;
    BusRW<uint16, true>(A, tmp, BurstHax, SH2DMAHax);

    tmp = V >> 0;
    BusRW<uint16, true>(A | 2, tmp, BurstHax, SH2DMAHax);
   }
   else
   {
    uint16 tmp = 0;

    BusRW<uint16, false>(A | 2, tmp, BurstHax, SH2DMAHax);
    V = tmp << 0;

    BusRW<uint16, false>(A, tmp, BurstHax, SH2DMAHax);
    V |= tmp << 16;
   }

   return;
  }

  //
  // Low(and kinda slow) work RAM 
  //
  if(A >= 0x00200000 && A <= 0x003FFFFF)
  {
   ne16_rwbo_be<T, IsWrite>(WorkRAML, A & 0xFFFFF, &V);

   if(!SH2DMAHax)
    SH7095_mem_timestamp += 7;
   else
    *SH2DMAHax -= 7;

   return;
  }

  //
  // BIOS ROM
  //
  if(A >= 0x00000000 && A <= 0x000FFFFF)
  {
   if(!SH2DMAHax)
    SH7095_mem_timestamp += 8;
   else
    *SH2DMAHax -= 8;

   if(!IsWrite) 
    V = ne16_rbo_be<T>(BIOSROM, A & 0x7FFFF);

   return;
  }

  //
  // SMPC
  //
  if(A >= 0x00100000 && A <= 0x0017FFFF)
  {
   const uint32 SMPC_A = (A & 0x7F) >> 1;

   //if(!SH2DMAHax)
   // SH7095_mem_timestamp += 2;
   //

   if(IsWrite)
   {
    if(sizeof(T) == 2 || (A & 1))
     SMPC_Write(SH7095_mem_timestamp, SMPC_A, V);
   }
   else
   {
    if(sizeof(T) == 2)
     V = 0xFF00 | SMPC_Read(SH7095_mem_timestamp, SMPC_A);
    else if(sizeof(T) == 1 && (A & 1))
     V = SMPC_Read(SH7095_mem_timestamp, SMPC_A);
    else
     V = 0xFF;
   }

   return;
  }

  //
  // Backup RAM
  //
  if(A >= 0x00180000 && A <= 0x001FFFFF)
  {
   if(!SH2DMAHax)
    SH7095_mem_timestamp += 8;
   else
    *SH2DMAHax -= 8;

   if(IsWrite)
   {
    if(sizeof(T) != 1 || (A & 1))
    {
     BackupRAM[(A >> 1) & 0x7FFF] = V;
     BackupRAM_Dirty = true;
    }
   }
   else
    V = ((BackupRAM[(A >> 1) & 0x7FFF] << 0) | (0xFF << 8)) >> (((A & 1) ^ (sizeof(T) & 1)) << 3);  

   return;
  }

  //
  // FRT trigger region
  //
  if(A >= 0x01000000 && A <= 0x01FFFFFF)
  {
   if(!SH2DMAHax)
    SH7095_mem_timestamp += 8;
   else
    *SH2DMAHax -= 8;

   //printf("FT FRT %zu %08x %04x %d\n", sizeof(T), A, V, SMPC_IsSlaveOn());

   if(IsWrite)
   {
    if(sizeof(T) != 1)
    {
     const unsigned c = ((A >> 23) & 1) ^ 1;

     if(!c || SMPC_IsSlaveOn())
     {
      CPU[c].SetFTI(true);
      CPU[c].SetFTI(false);
     }
    }
   }
   return;
  }


  //
  //
  //
   if(!SH2DMAHax)
    SH7095_mem_timestamp += 4;
   else
    *SH2DMAHax -= 4;

  if(IsWrite)
   SS_DBG(SS_DBG_WARNING, "[SH2 BUS] Unknown %zu-byte write of 0x%08x to 0x%08x\n", sizeof(T), V, A);
  else
  {
   SS_DBG(SS_DBG_WARNING, "[SH2 BUS] Unknown %zu-byte read from 0x%08x\n", sizeof(T), A);

   V = 0;
  }

  return;
 }

 //
 // SCU
 //
 {
  uint32 DB;

  if(IsWrite)
   DB = V << (((A & 3) ^ (4 - sizeof(T))) << 3); 
  else
   DB = 0;

  SCU_FromSH2_BusRW_DB<T, IsWrite>(A, &DB, SH2DMAHax);

  if(!IsWrite)
   V = DB >> (((A & 3) ^ (4 - sizeof(T))) << 3); 
 }
}

template<typename T>
static void INLINE SH7095_BusWrite(uint32 A, T V, const bool BurstHax, int32* SH2DMAHax)
{
 BusRW<T, true>(A, V, BurstHax, SH2DMAHax);
}

template<typename T>
static T INLINE SH7095_BusRead(uint32 A, const bool BurstHax, int32* SH2DMAHax)
{
 T ret = 0;

 BusRW<T, false>(A, ret, BurstHax, SH2DMAHax);

 return ret;
}

//
//
//
static MDFN_COLD uint8 CheatMemRead(uint32 A)
{
 A &= (1U << 27) - 1;

 #ifdef MSB_FIRST
 return *(uint8*)(SH7095_FastMap[A >> SH7095_EXT_MAP_GRAN_BITS] + (A ^ 0));
 #else
 return *(uint8*)(SH7095_FastMap[A >> SH7095_EXT_MAP_GRAN_BITS] + (A ^ 1));
 #endif
}

static MDFN_COLD void CheatMemWrite(uint32 A, uint8 V)
{
 A &= (1U << 27) - 1;

 if(FMIsWriteable[A >> SH7095_EXT_MAP_GRAN_BITS])
 {
  #ifdef MSB_FIRST
  *(uint8*)(SH7095_FastMap[A >> SH7095_EXT_MAP_GRAN_BITS] + (A ^ 0)) = V;
  #else
  *(uint8*)(SH7095_FastMap[A >> SH7095_EXT_MAP_GRAN_BITS] + (A ^ 1)) = V;
  #endif

  for(unsigned c = 0; c < 2; c++)
  {
   if(CPU[c].CCR & SH7095::CCR_CE)
   {
    for(uint32 Abase = 0x00000000; Abase < 0x20000000; Abase += 0x08000000)
    {
     CPU[c].Write_UpdateCache<uint8>(Abase + A, V);
    }
   }
  }
 }
}
//
//
//
static void SetFastMemMap(uint32 Astart, uint32 Aend, uint16* ptr, uint32 length, bool is_writeable)
{
 const uint64 Abound = (uint64)Aend + 1;
 assert((Astart & ((1U << SH7095_EXT_MAP_GRAN_BITS) - 1)) == 0);
 assert((Abound & ((1U << SH7095_EXT_MAP_GRAN_BITS) - 1)) == 0);
 assert((length & ((1U << SH7095_EXT_MAP_GRAN_BITS) - 1)) == 0);
 assert(length > 0);
 assert(length <= (Abound - Astart));

 for(uint64 A = Astart; A < Abound; A += (1U << SH7095_EXT_MAP_GRAN_BITS))
 {
  uintptr_t tmp = (uintptr_t)ptr + ((A - Astart) % length);

  if(A < (1U << 27))
   FMIsWriteable[A >> SH7095_EXT_MAP_GRAN_BITS] = is_writeable;

  SH7095_FastMap[A >> SH7095_EXT_MAP_GRAN_BITS] = tmp - A;
 }
}

static uint16 fmap_dummy[(1U << SH7095_EXT_MAP_GRAN_BITS) / sizeof(uint16)];

static MDFN_COLD void InitFastMemMap(void)
{
 for(unsigned i = 0; i < sizeof(fmap_dummy) / sizeof(fmap_dummy[0]); i++)
 {
  fmap_dummy[i] = 0;
 }

 FMIsWriteable.reset();
 MDFNMP_Init(1ULL << SH7095_EXT_MAP_GRAN_BITS, (1ULL << 27) / (1ULL << SH7095_EXT_MAP_GRAN_BITS));

 for(uint64 A = 0; A < 1ULL << 32; A += (1U << SH7095_EXT_MAP_GRAN_BITS))
 {
  SH7095_FastMap[A >> SH7095_EXT_MAP_GRAN_BITS] = (uintptr_t)fmap_dummy - A;
 }
}

void SS_SetPhysMemMap(uint32 Astart, uint32 Aend, uint16* ptr, uint32 length, bool is_writeable)
{
 assert(Astart < 0x20000000);
 assert(Aend < 0x20000000);

 if(!ptr)
 {
  ptr = fmap_dummy;
  length = sizeof(fmap_dummy);
 }

 for(uint32 Abase = 0; Abase < 0x40000000; Abase += 0x20000000)
  SetFastMemMap(Astart + Abase, Aend + Abase, ptr, length, is_writeable);
}

#include "mednafen/ss/sh7095.inc"

static bool Running;

struct event_list_entry
{
 uint32 which;
 sscpu_timestamp_t event_time;
 event_list_entry *prev;
 event_list_entry *next;
 sscpu_timestamp_t (*event_handler)(const sscpu_timestamp_t timestamp);
};

static event_list_entry events[SS_EVENT__COUNT];
static sscpu_timestamp_t next_event_ts;

template<unsigned c>
static sscpu_timestamp_t SH_DMA_EventHandler(sscpu_timestamp_t et)
{
 return CPU[c].DMA_Update(et);
}

//
//
//

static MDFN_COLD void InitEvents(void)
{
 for(unsigned i = 0; i < SS_EVENT__COUNT; i++)
 {
  events[i].which = i;

  if(i == SS_EVENT__SYNFIRST)
   events[i].event_time = 0;
  else if(i == SS_EVENT__SYNLAST)
   events[i].event_time = 0x7FFFFFFF;
  else
   events[i].event_time = 0; //SS_EVENT_DISABLED_TS;

  events[i].prev = (i > 0) ? &events[i - 1] : NULL;
  events[i].next = (i < (SS_EVENT__COUNT - 1)) ? &events[i + 1] : NULL;
 }

 events[SS_EVENT_SH2_M_DMA].event_handler = &SH_DMA_EventHandler<0>;
 events[SS_EVENT_SH2_S_DMA].event_handler = &SH_DMA_EventHandler<1>;

 events[SS_EVENT_SCU_DMA].event_handler = SCU_UpdateDMA;
 events[SS_EVENT_SCU_DSP].event_handler = SCU_UpdateDSP;

 events[SS_EVENT_SMPC].event_handler = SMPC_Update;

 events[SS_EVENT_VDP1].event_handler = VDP1::Update;
 events[SS_EVENT_VDP2].event_handler = VDP2::Update;

 events[SS_EVENT_CDB].event_handler = CDB_Update;

 events[SS_EVENT_SOUND].event_handler = SOUND_Update;

 events[SS_EVENT_MIDSYNC].event_handler = MidSync;
 events[SS_EVENT_MIDSYNC].event_time = SS_EVENT_DISABLED_TS;
}

static void RebaseTS(const sscpu_timestamp_t timestamp)
{
 for(unsigned i = 0; i < SS_EVENT__COUNT; i++)
 {
  if(i == SS_EVENT__SYNFIRST || i == SS_EVENT__SYNLAST)
   continue;

  assert(events[i].event_time > timestamp);

  if(events[i].event_time != SS_EVENT_DISABLED_TS)
   events[i].event_time -= timestamp;
 }

 next_event_ts = events[SS_EVENT__SYNFIRST].next->event_time;
}

void SS_SetEventNT(const int type, const sscpu_timestamp_t next_timestamp)
{
 assert(MDFN_LIKELY(type > SS_EVENT__SYNFIRST && type < SS_EVENT__SYNLAST));
 event_list_entry *e = &events[type];

 if(next_timestamp < e->event_time)
 {
  event_list_entry *fe = e;

  do
  {
   fe = fe->prev;
  }
  while(next_timestamp < fe->event_time);

  // Remove this event from the list, temporarily of course.
  e->prev->next = e->next;
  e->next->prev = e->prev;

  // Insert into the list, just after "fe".
  e->prev = fe;
  e->next = fe->next;
  fe->next->prev = e;
  fe->next = e;

  e->event_time = next_timestamp;
 }
 else if(next_timestamp > e->event_time)
 {
  event_list_entry *fe = e;

  do
  {
   fe = fe->next;
  } while(next_timestamp > fe->event_time);

  // Remove this event from the list, temporarily of course
  e->prev->next = e->next;
  e->next->prev = e->prev;

  // Insert into the list, just BEFORE "fe".
  e->prev = fe->prev;
  e->next = fe;
  fe->prev->next = e;
  fe->prev = e;

  e->event_time = next_timestamp;
 }

 next_event_ts = (Running ? events[SS_EVENT__SYNFIRST].next->event_time : 0);
}

// Called from debug.cpp too.
void ForceEventUpdates(const sscpu_timestamp_t timestamp)
{
 CPU[0].ForceInternalEventUpdates();

 if(SMPC_IsSlaveOn())
  CPU[1].ForceInternalEventUpdates();

 for(unsigned evnum = SS_EVENT__SYNFIRST + 1; evnum < SS_EVENT__SYNLAST; evnum++)
 {
  if(events[evnum].event_time != SS_EVENT_DISABLED_TS)
   SS_SetEventNT(evnum, events[evnum].event_handler(timestamp));
 }

 next_event_ts = (Running ? events[SS_EVENT__SYNFIRST].next->event_time : 0);
}

static INLINE bool EventHandler(const sscpu_timestamp_t timestamp)
{
 event_list_entry *e;

 while(timestamp >= (e = events[SS_EVENT__SYNFIRST].next)->event_time)	// If Running = 0, EventHandler() may be called even if there isn't an event per-se, so while() instead of do { ... } while
 {
  const sscpu_timestamp_t etime = e->event_time;
  sscpu_timestamp_t nt;

  nt = e->event_handler(e->event_time);	// timestamp

//#if SS_EVENT_SYSTEM_CHECKS
  if(MDFN_UNLIKELY(nt <= etime))
  {
   fprintf(stderr, "which=%d event_time=%d nt=%d timestamp=%d\n", e->which, etime, nt, timestamp);
   assert(nt > etime);
  }
//#endif

  SS_SetEventNT(e->which, nt);
 }

 return(Running);
}

void SS_RequestMLExit(void)
{
 Running = 0;
 next_event_ts = 0;
}

#pragma GCC push_options
#pragma GCC optimize("Os,no-unroll-loops,no-peel-loops,no-crossjumping")
template<bool DebugMode>
static int32 NO_INLINE RunLoop(EmulateSpecStruct* espec)
{
 sscpu_timestamp_t eff_ts = 0;

 //printf("%d %d\n", SH7095_mem_timestamp, CPU[0].timestamp);

 do
 {
  do
  {
#ifdef HAVE_DEBUG
   if(DebugMode)
    DBG_CPUHandler<0>(eff_ts);
#endif

   CPU[0].Step<0, DebugMode>();

   while(MDFN_LIKELY(CPU[0].timestamp > CPU[1].timestamp))
   {
#ifdef HAVE_DEBUG
    if(DebugMode)
     DBG_CPUHandler<1>(eff_ts);
#endif

    CPU[1].Step<1, DebugMode>();
   }

   eff_ts = CPU[0].timestamp;
   if(SH7095_mem_timestamp > eff_ts)
    eff_ts = SH7095_mem_timestamp;
   else
    SH7095_mem_timestamp = eff_ts;
  } while(MDFN_LIKELY(eff_ts < next_event_ts));
 } while(MDFN_LIKELY(EventHandler(eff_ts)));

 //printf(" End: %d %d -- %d\n", SH7095_mem_timestamp, CPU[0].timestamp, eff_ts);
 return eff_ts;
}
#pragma GCC pop_options

void SS_Reset(bool powering_up)
{
 SH7095_BusLock = 0;

 if(powering_up)
 {
  memset(WorkRAML, 0x00, sizeof(WorkRAML));	// TODO: Check
  memset(WorkRAMH, 0x00, sizeof(WorkRAMH));	// TODO: Check
 }

 if(powering_up)
 {
  CPU[0].TruePowerOn();
  CPU[1].TruePowerOn();
 }

 SCU_Reset(powering_up);
 CPU[0].Reset(powering_up);

 SMPC_Reset(powering_up);

 VDP1::Reset(powering_up);
 VDP2::Reset(powering_up);

 CDB_Reset(powering_up);

 SOUND_Reset(powering_up);

 CART_Reset(powering_up);
}

static EmulateSpecStruct* espec;
static bool AllowMidSync = false;
static int32 cur_clock_div;
static sscpu_timestamp_t MidSync(const sscpu_timestamp_t timestamp)
{
 if(AllowMidSync)
 {
  //
  // Don't call SOUND_Update() here, it's not necessary and will subtly alter emulation behavior from the perspective of the emulated program
  // (which is not a problem in and of itself, but it's preferable to keep settings from altering emulation behavior when they don't need to).
  //
  //printf("MidSync: %d\n", VDP2::PeekLine());
  {
   espec->SoundBufSize += SOUND_FlushOutput(espec->SoundBuf + (espec->SoundBufSize * 2), espec->SoundBufMaxSize - espec->SoundBufSize, false);
   espec->MasterCycles = timestamp * cur_clock_div;
  }
  //printf("%d\n", espec->SoundBufSize);
  //
  //
  //MDFN_MidSync(espec);
  //
  //
  SMPC_UpdateInput();
  AllowMidSync = false;
 }

 return SS_EVENT_DISABLED_TS;
}

static void Emulate(EmulateSpecStruct* espec_arg)
{
 int32 end_ts;

 espec = espec_arg;
 AllowMidSync = MDFN_GetSettingB("ss.midsync");
 MDFNGameInfo->mouse_sensitivity = MDFN_GetSettingF("ss.input.mouse_sensitivity");

 printf("Emulate 0.\n");
 cur_clock_div = SMPC_StartFrame(espec);
 printf("Emulate 1.\n");
 SMPC_UpdateInput();
 printf("Emulate 2.\n");
 VDP2::StartFrame(espec, cur_clock_div == 61);
 SOUND_StartFrame(espec->SoundRate / espec->soundmultiplier, MDFN_GetSettingUI("ss.scsp.resamp_quality"));
 espec->SoundBufSize = 0;
 espec->MasterCycles = 0;
 espec->soundmultiplier = 1;
 //
 //
 //
 Running = true;	// Set before ForceEventUpdates()
 ForceEventUpdates(0);

#ifdef WANT_DEBUGGER
 if(DBG_NeedCPUHooks())
  end_ts = RunLoop<true>(espec);
 else
#endif
  end_ts = RunLoop<false>(espec);

 ForceEventUpdates(end_ts);
 //
 //
 //
 RebaseTS(end_ts);

 CDB_ResetTS();
 SOUND_ResetTS();
 VDP1::ResetTS();
 VDP2::ResetTS();
 SMPC_ResetTS();
 SCU_ResetTS();

 if(!(SH7095_mem_timestamp & 0x40000000))	// or maybe >= 0 instead?
  SH7095_mem_timestamp -= end_ts;

 CPU[0].AdjustTS(-end_ts);

 if(SMPC_IsSlaveOn())
  CPU[1].AdjustTS(-end_ts);
 //
 //
 //
 espec->MasterCycles = end_ts * cur_clock_div;
 espec->SoundBufSize += SOUND_FlushOutput(espec->SoundBuf + (espec->SoundBufSize * 2), espec->SoundBufMaxSize - espec->SoundBufSize, espec->NeedSoundReverse);
 espec->NeedSoundReverse = false;
 //
 //
 //
 if(BackupRAM_Dirty)
 {
  BackupRAM_SaveDelay = (int64)3 * (EmulatedSS.MasterClock / MDFN_MASTERCLOCK_FIXED(1));	// 3 second delay
  BackupRAM_Dirty = false;
 }
 else if(BackupRAM_SaveDelay > 0)
 {
  BackupRAM_SaveDelay -= espec->MasterCycles;

  if(BackupRAM_SaveDelay <= 0)
  {
   try
   {
    SaveBackupRAM();
   }
   catch(std::exception& e)
   {
    MDFN_DispMessage("%s", e.what());
    BackupRAM_SaveDelay = (int64)60 * (EmulatedSS.MasterClock / MDFN_MASTERCLOCK_FIXED(1));	// 60 second retry delay.
   }
  }
 }

 if(CART_GetClearNVDirty())
  CartNV_SaveDelay = (int64)3 * (EmulatedSS.MasterClock / MDFN_MASTERCLOCK_FIXED(1));	// 3 second delay
 else if(CartNV_SaveDelay > 0)
 {
  CartNV_SaveDelay -= espec->MasterCycles;

  if(CartNV_SaveDelay <= 0)
  {
   try
   {
    SaveCartNV();
   }
   catch(std::exception& e)
   {
    MDFN_DispMessage("%s", e.what());
    CartNV_SaveDelay = (int64)60 * (EmulatedSS.MasterClock / MDFN_MASTERCLOCK_FIXED(1));	// 60 second retry delay.
   }
  }
 }
}

//
//
//

static MDFN_COLD void Cleanup(void)
{
 CART_Kill();

 DBG_Kill();
 VDP1::Kill();
 VDP2::Kill();
 SOUND_Kill();
 CDB_Kill();

 cdifs = NULL;
}

static bool IsSaturnDisc(const uint8* sa32k)
{
 if(sha256(&sa32k[0x100], 0xD00) != "96b8ea48819cfa589f24c40aa149c224c420dccf38b730f00156efe25c9bbc8f"_sha256)
  return false;

 if(memcmp(&sa32k[0], "SEGA SEGASATURN ", 16))
  return false;

 return true;
}

static void CalcGameID(uint8* id_out16, uint8* fd_id_out16, char* sgid)
{
   md5_context mctx;
   uint8_t *buf = new uint8_t[2048];

   mctx.starts();

   for(size_t x = 0; x < cdifs->size(); x++)
   {
      auto* c = (*cdifs)[x];
      TOC toc;

      c->ReadTOC(&toc);

      mctx.update_u32_as_lsb(toc.first_track);
      mctx.update_u32_as_lsb(toc.last_track);
      mctx.update_u32_as_lsb(toc.disc_type);

      for(unsigned i = 1; i <= 100; i++)
      {
         const auto& t = toc.tracks[i];

         mctx.update_u32_as_lsb(t.adr);
         mctx.update_u32_as_lsb(t.control);
         mctx.update_u32_as_lsb(t.lba);
         mctx.update_u32_as_lsb(t.valid);
      }

      for(unsigned i = 0; i < 512; i++)
      {
         if(c->ReadSector((uint8_t*)&buf[0], i, 1) >= 0x1)
         {
            if(i == 0)
            {
               char* tmp;
               memcpy(sgid, (void*)(buf[0x20]), 16);
               sgid[16] = 0;
               if((tmp = strrchr(sgid, 'V')))
               {
                  do
                  {
                     *tmp = 0;
                  } while(tmp-- != sgid && (signed char)*tmp <= 0x20);
               }
            }

            mctx.update(&buf[0], 2048);
         }
      }

      if(x == 0)
      {
         md5_context fd_mctx = mctx;
         fd_mctx.finish(fd_id_out16);
      }
   }

   free(buf);

   mctx.finish(id_out16);
}

//
// Remember to rebuild region database in db.cpp if changing the order of entries in this table(and be careful about game id collisions, e.g. with some Korean games).
//
static const struct
{
 const char c;
 const char* str;	// Community-defined region string that may appear in filename.
 unsigned region;
} region_strings[] =
{
 // Listed in order of preference for multi-region games.
 { 'U', "USA", SMPC_AREA_NA },
 { 'J', "Japan", SMPC_AREA_JP },
 { 'K', "Korea", SMPC_AREA_KR },

 { 'E', "Europe", SMPC_AREA_EU_PAL },
 { 'E', "Germany", SMPC_AREA_EU_PAL },
 { 'E', "France", SMPC_AREA_EU_PAL },
 { 'E', "Spain", SMPC_AREA_EU_PAL },

 { 'B', "Brazil", SMPC_AREA_CSA_NTSC },

 { 'T', nullptr, SMPC_AREA_ASIA_NTSC },
 { 'A', nullptr, SMPC_AREA_ASIA_PAL },
 { 'L', nullptr, SMPC_AREA_CSA_PAL },
};


static bool DetectRegion(unsigned* const region)
{
 uint8_t *buf = new uint8[2048 * 16];
 uint64 possible_regions = 0;

 for(auto& c : *cdifs)
 {
  if(c->ReadSector(&buf[0], 0, 16) != 0x1)
   continue;

  if(!IsSaturnDisc(&buf[0]))
   continue;

  for(unsigned i = 0; i < 16; i++)
  {
   for(auto const& rs : region_strings)
   {
    if(rs.c == buf[0x40 + i])
    {
     possible_regions |= (uint64)1 << rs.region;
     break;
    }
   }
  }
  break;
 }

 free(buf);

 for(auto const& rs : region_strings)
 {
  if(possible_regions & ((uint64)1 << rs.region))
  {
   *region = rs.region;
   return true;
  }
 }

 return false;
}

static bool DetectRegionByFN(const std::string& fn, unsigned* const region)
{
 std::string ss = fn;
 size_t cp_pos;
 uint64 possible_regions = 0;

 while((cp_pos = ss.rfind(')')) != std::string::npos && cp_pos > 0)
 {
  ss.resize(cp_pos);
  //
  size_t op_pos = ss.rfind('(');

  if(op_pos != std::string::npos)
  {
   for(auto const& rs : region_strings)
   {
    if(!rs.str)
     continue;

    size_t rs_pos = ss.find(rs.str, op_pos + 1);

    if(rs_pos != std::string::npos)
    {
     bool leading_ok = true;
     bool trailing_ok = true;

     for(size_t i = rs_pos - 1; i > op_pos; i--)
     {
      if(ss[i] == ',')
       break;
      else if(ss[i] != ' ')
      {
       leading_ok = false;
       break;
      }
     }

     for(size_t i = rs_pos + strlen(rs.str); i < ss.size(); i++)
     {
      if(ss[i] == ',')
       break;
      else if(ss[i] != ' ')
      {
       trailing_ok = false;
       break;
      }
     }

     if(leading_ok && trailing_ok)
      possible_regions |= (uint64)1 << rs.region;
    }
   }
  }
 }

 for(auto const& rs : region_strings)
 {
  if(possible_regions & ((uint64)1 << rs.region))
  {
   *region = rs.region;
   return true;
  }
 }

 return false;
}

static bool InitCommon(const unsigned cart_type, const unsigned smpc_area)
{
 ss_dbg_mask = MDFN_GetSettingUI("ss.dbg_mask");
 //
 {
  printf(_("Region: 0x%01x\n"), smpc_area);
  const struct
  {
   const unsigned type;
   const char* name;
  } CartNames[] =
  {
   { CART_NONE, _("None") },
   { CART_BACKUP_MEM, _("Backup Memory") },
   { CART_EXTRAM_1M, _("1MiB Extended RAM") },
   { CART_EXTRAM_4M, _("4MiB Extended RAM") },
   { CART_KOF95, _("King of Fighters '95 ROM") },
   { CART_ULTRAMAN, _("Ultraman ROM") },
   { CART_MDFN_DEBUG, _("Mednafen Debug") }, 
  };
  const char* cn = _("Unknown");

  for(auto const& cne : CartNames)
  {
   if(cne.type == cart_type)
   {
    cn = cne.name;
    break;
   }
  }
  printf(_("Cart: %s\n"), cn);
 }
 //

 for(unsigned c = 0; c < 2; c++)
 {
  CPU[c].Init();
  CPU[c].SetMD5((bool)c);
 }

 //
 // Initialize backup memory.
 // 
 memset(BackupRAM, 0x00, sizeof(BackupRAM));
 for(unsigned i = 0; i < 0x40; i++)
  BackupRAM[i] = BRAM_Init_Data[i & 0x0F];

 // Call InitFastMemMap() before functions like SOUND_Init()
 InitFastMemMap();
 SS_SetPhysMemMap(0x00000000, 0x000FFFFF, BIOSROM, sizeof(BIOSROM));
 SS_SetPhysMemMap(0x00200000, 0x003FFFFF, WorkRAML, sizeof(WorkRAML), true);
 SS_SetPhysMemMap(0x06000000, 0x07FFFFFF, WorkRAMH, sizeof(WorkRAMH), true);
 MDFNMP_RegSearchable(0x00200000, sizeof(WorkRAML));
 MDFNMP_RegSearchable(0x06000000, sizeof(WorkRAMH));

 CART_Init(cart_type);

 //
 //
 //
 const bool PAL = (smpc_area & SMPC_AREA__PAL_MASK);
 const int32 MasterClock = PAL ? 1734687500 : 1746818182;	// NTSC: 1746818181.8181818181, PAL: 1734687500-ish
 const char* biospath_sname;
 int sls = MDFN_GetSettingI(PAL ? "ss.slstartp" : "ss.slstart");
 int sle = MDFN_GetSettingI(PAL ? "ss.slendp" : "ss.slend");

 if(sls > sle)
  std::swap(sls, sle);

 if(smpc_area == SMPC_AREA_JP || smpc_area == SMPC_AREA_ASIA_NTSC)
  biospath_sname = "ss.bios_jp";
 else
  biospath_sname = "ss.bios_na_eu";

 {
  const std::string biospath = MDFN_MakeFName(MDFNMKF_FIRMWARE, 0, MDFN_GetSettingS(biospath_sname).c_str());
  FileStream BIOSFile(biospath.c_str(), MODE_READ);

  if(BIOSFile.size() != 524288)
  {
   printf("BIOS file \"%s\" is of an incorrect size.\n", biospath.c_str());
   return false;
  }

  BIOSFile.read(BIOSROM, 512 * 1024);
  BIOS_SHA256 = sha256(BIOSROM, 512 * 1024);

  if(MDFN_GetSettingB("ss.bios_sanity"))
  {
   static const struct
   {
    const char* fn;
    sha256_digest hash;
    const uint32 areas;
   } BIOSDB[] =
   {
    { "sega1003.bin",  "cc1e1b7f88f1c6e6fc35994bae2c2292e06fdae258c79eb26a1f1391e72914a8"_sha256, (1U << SMPC_AREA_JP) | (1U << SMPC_AREA_ASIA_NTSC),  },
    { "sega_100.bin",  "ae4058627bb5db9be6d8d83c6be95a4aa981acc8a89042e517e73317886c8bc2"_sha256, (1U << SMPC_AREA_JP) | (1U << SMPC_AREA_ASIA_NTSC),  },
    { "sega_101.bin",  "dcfef4b99605f872b6c3b6d05c045385cdea3d1b702906a0ed930df7bcb7deac"_sha256, (1U << SMPC_AREA_JP) | (1U << SMPC_AREA_ASIA_NTSC),  },
    { "sega_100a.bin", "87293093fad802fcff31fcab427a16caff1acbc5184899b8383b360fd58efb73"_sha256, (~0U) & ~((1U << SMPC_AREA_JP) | (1U << SMPC_AREA_ASIA_NTSC)) },
    { "mpr-17933.bin", "96e106f740ab448cf89f0dd49dfbac7fe5391cb6bd6e14ad5e3061c13330266f"_sha256, (~0U) & ~((1U << SMPC_AREA_JP) | (1U << SMPC_AREA_ASIA_NTSC)) },
   };
   std::string fnbase, fnext;
   std::string fn;

   MDFN_GetFilePathComponents(biospath, nullptr, &fnbase, &fnext);
   fn = fnbase + fnext;

   for(auto const& dbe : BIOSDB)
   {
    if(BIOS_SHA256 == dbe.hash)
    {
     if(!(dbe.areas & (1U << smpc_area)))
     {
        printf("Wrong BIOS for region being emulated.\n");
        return false;
     }
    }
    else if(fn == dbe.fn)	// Discourage people from renaming files instead of changing settings.
    {
       printf("BIOS hash does not match that as expected by filename.\n");
       return false;
    }
   }
  }
  //
  //
  for(unsigned i = 0; i < 262144; i++)
   BIOSROM[i] = MDFN_de16msb((const uint8_t*)&BIOSROM[i]);
 }

 EmulatedSS.MasterClock = MDFN_MASTERCLOCK_FIXED(MasterClock);

 SCU_Init();
 SMPC_Init(smpc_area, MasterClock);
 VDP1::Init();
 VDP2::Init(PAL, sls, sle);
 VDP2::FillVideoParams(&EmulatedSS);
 CDB_Init();
 SOUND_Init();

 InitEvents();

 DBG_Init();
 //
 //
 //
 try { LoadRTC();       } catch(MDFN_Error& e) { if(e.GetErrno() != ENOENT) throw; }
 try { LoadBackupRAM(); } catch(MDFN_Error& e) { if(e.GetErrno() != ENOENT) throw; }
 try { LoadCartNV();    } catch(MDFN_Error& e) { if(e.GetErrno() != ENOENT) throw; }

 BackupBackupRAM();
 BackupCartNV();

 BackupRAM_Dirty = false;
 BackupRAM_SaveDelay = 0;

 CART_GetClearNVDirty();
 CartNV_SaveDelay = 0;
 //
 if(MDFN_GetSettingB("ss.smpc.autortc"))
 {
  time_t ut;
  struct tm* ht;

  if((ut = time(NULL)) == (time_t)-1)
  {
     printf("AutoRTC error #1\n");
     return false;
  }

  if((ht = localtime(&ut)) == NULL)
  {
     printf("AutoRTC error #2\n");
     return false;
  }

  SMPC_SetRTC(ht, MDFN_GetSettingUI("ss.smpc.autortc.lang"));
 }
 //
 SS_Reset(true);

 return true;
}

static bool TestMagic(MDFNFILE* fp)
{
 return false;
}

static bool Load(MDFNFILE* fp)
{
#if 0
   // cat regiondb.inc | sort | uniq --all-repeated=separate -w 102 
   {
      FileStream rdbfp("/tmp/regiondb.inc", MODE_WRITE);
      Stream* s = fp->stream();
      std::string linebuf;
      static std::vector<CDIF *> CDInterfaces;

      cdifs = &CDInterfaces;

      while(s->get_line(linebuf) >= 0)
      {
         static uint8 sbuf[2048 * 16];
         CDIF* iface = CDIF_Open(linebuf, false);
         int m = iface->ReadSector(sbuf, 0, 16, true);
         std::string fb;

         assert(m == 0x1); 
         assert(IsSaturnDisc(&sbuf[0]) == true);
         //
         uint8 dummytmp[16] = { 0 };
         uint8 tmp[16] = { 0 };
         const char* regstr;
         unsigned region = ~0U;

         MDFN_GetFilePathComponents(linebuf, nullptr, &fb);

         if(!DetectRegionByFN(fb, &region))
            abort();

         switch(region)
         {
            default: abort(); break;
            case SMPC_AREA_NA: regstr = "SMPC_AREA_NA"; break;
            case SMPC_AREA_JP: regstr = "SMPC_AREA_JP"; break;
            case SMPC_AREA_EU_PAL: regstr = "SMPC_AREA_EU_PAL"; break;
            case SMPC_AREA_KR: regstr = "SMPC_AREA_KR"; break;
            case SMPC_AREA_CSA_NTSC: regstr = "SMPC_AREA_CSA_NTSC"; break;
         }

         CDInterfaces.clear();
         CDInterfaces.push_back(iface);

         CalcGameID(dummytmp, tmp);

         unsigned tmpreg;
         if(!DetectRegion(&tmpreg) || tmpreg != region)
         {
            rdbfp.print_format("{ { ");
            for(unsigned i = 0; i < 16; i++)
               rdbfp.print_format("0x%02x, ", tmp[i]);
            rdbfp.print_format("}, %s }, // %s\n", regstr, fb.c_str());
         }

         delete iface;
      }
   }

   return;
#endif
   //uint8 elf_header[

   cdifs = NULL;

   unsigned i;
   if(MDFN_GetSettingS("ss.dbg_exe_cdpath") != "")
   {
      bool success;
      RMD_Drive dr;

      dr.Name = std::string("Virtual CD Drive");
      dr.PossibleStates.push_back(RMD_State({"Tray Open", false, false, true}));
      dr.PossibleStates.push_back(RMD_State({"Tray Closed (Empty)", false, false, false}));
      dr.PossibleStates.push_back(RMD_State({"Tray Closed", true, true, false}));
      dr.CompatibleMedia.push_back(0);
      dr.MediaMtoPDelay = 2000;

      MDFNGameInfo->RMD->Drives.push_back(dr);
      MDFNGameInfo->RMD->MediaTypes.push_back(RMD_MediaType({"CD"}));
      MDFNGameInfo->RMD->Media.push_back(RMD_Media({"Test CD", 0}));

      static std::vector<CDIF *> CDInterfaces;
      CDInterfaces.clear();
      CDInterfaces.push_back(CDIF_Open(&success, MDFN_GetSettingS("ss.dbg_exe_cdpath").c_str(), false,
               false));
      cdifs = &CDInterfaces;
   }

   if (!InitCommon(CART_MDFN_DEBUG, MDFN_GetSettingUI("ss.region_default")))
      return false;

   // 0x25FE00C4 = 0x1;
   for(i = 0; i < fp->size; i += 2)
   {
      uint8 tmp[2];

      file_read(fp, tmp, 2, 1);
      //fp->read(tmp, 2);

      *(uint16*)((uint8*)WorkRAMH + 0x4000 + i) = (tmp[0] << 8) | (tmp[1] << 0);
   }
   BIOSROM[0] = 0x0600;
   BIOSROM[1] = 0x4000; //0x4130; //0x4060;

   BIOSROM[2] = 0x0600;
   BIOSROM[3] = 0x4000; //0x4130; //0x4060;

   BIOSROM[4] = 0xDEAD;
   BIOSROM[5] = 0xBEEF;

   return true;
}

static MDFN_COLD bool TestMagicCD(std::vector<CDIF *> *CDInterfaces)
{
   bool is_cd = false;
   uint8 *buf = new uint8[2048 * 16];

   if((*CDInterfaces)[0]->ReadSector(&buf[0], 0, 16) != 0x1)
      return false;

   is_cd = IsSaturnDisc(&buf[0]);

   free(buf);

   return is_cd;
}

static bool DiscSanityChecks(void)
{
   size_t i;

   for(i = 0; i < cdifs->size(); i++)
   {
      TOC toc;

      (*cdifs)[i]->ReadTOC(&toc);

      for(int32 track = 1; track <= 99; track++)
      {
         if(!toc.tracks[track].valid)
            continue;

         if(toc.tracks[track].control & SUBQ_CTRLF_DATA)
            continue;
         //
         //
         //
         const int32 start_lba = toc.tracks[track].lba;
         const int32 end_lba = start_lba + 32 - 1;
         bool any_subq_curpos = false;

         for(int32 lba = start_lba; lba <= end_lba; lba++)
         {
            uint8 pwbuf[96];
            uint8 qbuf[12];

            if(!(*cdifs)[i]->ReadRawSectorPWOnly(pwbuf, lba, false))
            {
               printf("Disc %zu of %zu: Error reading sector at lba=%d in DiscSanityChecks().\n", i + 1, cdifs->size(), lba);
               return false;
            }

            subq_deinterleave(pwbuf, qbuf);
            if(subq_check_checksum(qbuf) && (qbuf[0] & 0xF) == ADR_CURPOS)
            {
               const uint8 qm = qbuf[7];
               const uint8 qs = qbuf[8];
               const uint8 qf = qbuf[9];
               uint8 lm, ls, lf;

               any_subq_curpos = true;

               LBA_to_AMSF(lba, &lm, &ls, &lf);
               lm = U8_to_BCD(lm);
               ls = U8_to_BCD(ls);
               lf = U8_to_BCD(lf);

               if(lm != qm || ls != qs || lf != qf)
               {
                  printf("Disc %zu of %zu: Time mismatch at lba=%d(%02x:%02x:%02x); Q subchannel: %02x:%02x:%02x\n",
                        i + 1, cdifs->size(),
                        lba,
                        lm, ls, lf,
                        qm, qs, qf);
                  return false;
               }
            }
         }

         if(!any_subq_curpos)
         {
            printf("Disc %zu of %zu: No valid Q subchannel ADR_CURPOS data present at lba %d-%d?!\n", i + 1, cdifs->size(), start_lba, end_lba);
            return false;
         }

         break;
      }
   }

   return true;
}

static MDFN_COLD bool LoadCD(std::vector<CDIF *>* CDInterfaces)
{
   const unsigned region_default = MDFN_GetSettingI("ss.region_default");
   unsigned region = region_default;
   int cart_type;
   uint8 fd_id[16];
   char sgid[16 + 1];
   cdifs = CDInterfaces;
   CalcGameID(MDFNGameInfo->MD5, fd_id, sgid);

   if(MDFN_GetSettingB("ss.region_autodetect"))
      if(!DB_LookupRegionDB(fd_id, &region))
         DetectRegion(&region);
   //
   //
   if((cart_type = MDFN_GetSettingI("ss.cart")) == CART__RESERVED)
   {
      cart_type = CART_BACKUP_MEM;
      DB_LookupCartDB(sgid, &cart_type);
   }

   if(MDFN_GetSettingB("ss.cd_sanity"))
   {
      if (!DiscSanityChecks())
         return false;
   }
   else
      printf(_("WARNING: CD (image) sanity checks disabled."));

   // TODO: auth ID calc


   if (!InitCommon(cart_type, region))
      return false;

   return true;
}

static MDFN_COLD void CloseGame(void)
{
#if 0
 VDP1::MakeDump("/tmp/vdp1_dump.h");
 VDP2::MakeDump("/tmp/vdp2_dump.h");
#endif
 //
 //

 SaveBackupRAM();
 SaveCartNV();
 SaveRTC();

 Cleanup();
}

static MDFN_COLD void SaveBackupRAM(void)
{
 FileStream brs(MDFN_MakeFName(MDFNMKF_SAV, 0, "bkr"), MODE_WRITE_INPLACE);

 brs.write(BackupRAM, sizeof(BackupRAM));

 brs.close();
}

static MDFN_COLD void LoadBackupRAM(void)
{
 FileStream brs(MDFN_MakeFName(MDFNMKF_SAV, 0, "bkr"), MODE_READ);

 brs.read(BackupRAM, sizeof(BackupRAM));
}

static MDFN_COLD void BackupBackupRAM(void)
{
 MDFN_BackupSavFile(10, "bkr");
}

static MDFN_COLD void BackupCartNV(void)
{
 const char* ext = nullptr;
 void* nv_ptr = nullptr;
 uint64 nv_size = 0;

 CART_GetNVInfo(&ext, &nv_ptr, &nv_size);

 if(ext)
  MDFN_BackupSavFile(10, ext);
}

static MDFN_COLD void LoadCartNV(void)
{
 const char* ext = nullptr;
 void* nv_ptr = nullptr;
 uint64 nv_size = 0;

 CART_GetNVInfo(&ext, &nv_ptr, &nv_size);

 if(ext)
 {
  FileStream nvs(MDFN_MakeFName(MDFNMKF_SAV, 0, ext), MODE_READ);

  nvs.read(nv_ptr, nv_size);
 }
}

static MDFN_COLD void SaveCartNV(void)
{
 const char* ext = nullptr;
 void* nv_ptr = nullptr;
 uint64 nv_size = 0;

 CART_GetNVInfo(&ext, &nv_ptr, &nv_size);

 if(ext)
 {
  FileStream nvs(MDFN_MakeFName(MDFNMKF_SAV, 0, ext), MODE_WRITE_INPLACE);

  nvs.write(nv_ptr, nv_size);

  nvs.close();
 }
}

static MDFN_COLD void SaveRTC(void)
{
 FileStream sds(MDFN_MakeFName(MDFNMKF_SAV, 0, "smpc"), MODE_WRITE_INPLACE);

 SMPC_SaveNV(&sds);

 sds.close();
}

static MDFN_COLD void LoadRTC(void)
{
 FileStream sds(MDFN_MakeFName(MDFNMKF_SAV, 0, "smpc"), MODE_READ);

 SMPC_LoadNV(&sds);
}

int StateAction(StateMem *sm, int load, int data_only)
{
   return 0;
}

static MDFN_COLD bool SetMedia(uint32 drive_idx, uint32 state_idx, uint32 media_idx, uint32 orientation_idx)
{
 const RMD_Layout* rmd = EmulatedSS.RMD;
 const RMD_Drive* rd = &rmd->Drives[drive_idx];
 const RMD_State* rs = &rd->PossibleStates[state_idx];

 //printf("%d %d %d\n", rs->MediaPresent, rs->MediaUsable, rs->MediaCanChange);

 if(rs->MediaPresent && rs->MediaUsable)
  CDB_SetDisc(false, (*cdifs)[media_idx]);
 else
  CDB_SetDisc(rs->MediaCanChange, NULL);

 return(true);
}

static void DoSimpleCommand(int cmd)
{
 switch(cmd)
 {
  case MDFN_MSC_POWER: SS_Reset(true); break;
  // MDFN_MSC_RESET is not handled here; special reset button handling in smpc.cpp.
 }
}

static const FileExtensionSpecStruct KnownExtensions[] =
{
 { ".elf", "SS Homebrew ELF Executable" },

 { NULL, NULL }
};

static const MDFNSetting_EnumList Region_List[] =
{
 { "jp", SMPC_AREA_JP, "Japan" },
 { "na", SMPC_AREA_NA, "North America" },
 { "eu", SMPC_AREA_EU_PAL, "Europe" },
 { "kr", SMPC_AREA_KR, "South Korea" },

 { "tw", SMPC_AREA_ASIA_NTSC, "Taiwan" },	// Taiwan, Philippines
 { "as", SMPC_AREA_ASIA_PAL, "China" },	// China, Middle East

 { "br", SMPC_AREA_CSA_NTSC, "Brazil" },
 { "la", SMPC_AREA_CSA_PAL, "Latin America" },

 { NULL, 0 },
};

static const MDFNSetting_EnumList RTCLang_List[] =
{
 { "english", SMPC_RTC_LANG_ENGLISH, "English" },
 { "german", SMPC_RTC_LANG_GERMAN, "Deutsch" },
 { "french", SMPC_RTC_LANG_FRENCH, "Français" },
 { "spanish", SMPC_RTC_LANG_SPANISH, "Español" },
 { "italian", SMPC_RTC_LANG_ITALIAN, "Italiano" },
 { "japanese", SMPC_RTC_LANG_JAPANESE, "日本語" },

 { "deutsch", SMPC_RTC_LANG_GERMAN, NULL },
 { "français", SMPC_RTC_LANG_FRENCH, NULL },
 { "español", SMPC_RTC_LANG_SPANISH, NULL },
 { "italiano", SMPC_RTC_LANG_ITALIAN, NULL },
 { "日本語", SMPC_RTC_LANG_JAPANESE, NULL},

 { NULL, 0 },
};

static const MDFNSetting_EnumList Cart_List[] =
{
 { "auto", CART__RESERVED, "Automatic" },
 { "none", CART_NONE, "None" },
 { "backup", CART_BACKUP_MEM, "Backup Memory(512KiB)" },
 { "extram1", CART_EXTRAM_1M, "1MiB Extended RAM" },
 { "extram4", CART_EXTRAM_4M, "4MiB Extended RAM" },

 { NULL, 0 },
};

static MDFNSetting SSSettings[] =
{
 { "ss.bios_jp", MDFNSF_EMU_STATE, "Path to the Japan ROM BIOS", "", MDFNST_STRING, "sega_101.bin" },
 { "ss.bios_na_eu", MDFNSF_EMU_STATE, "Path to the North America and Europe ROM BIOS", "", MDFNST_STRING, "mpr-17933.bin" },

 { "ss.scsp.resamp_quality", MDFNSF_NOFLAGS, "SCSP output resampler quality.",
	"0 is lowest quality and CPU usage, 10 is highest quality and CPU usage.  The resampler that this setting refers to is used for converting from 44.1KHz to the sampling rate of the host audio device Mednafen is using.  Changing Mednafen's output rate, via the \"sound.rate\" setting, to \"44100\" may bypass the resampler, which can decrease CPU usage by Mednafen, and can increase or decrease audio quality, depending on various operating system and hardware factors.", MDFNST_UINT, "4", "0", "10" },

 { "ss.region_autodetect", MDFNSF_EMU_STATE | MDFNSF_UNTRUSTED_SAFE, "Attempt to auto-detect region of game.", NULL, MDFNST_BOOL, "1" },
 { "ss.region_default", MDFNSF_EMU_STATE | MDFNSF_UNTRUSTED_SAFE, "Default region to use.", "Used if region autodetection fails or is disabled.", MDFNST_ENUM, "jp", NULL, NULL, NULL, NULL, Region_List },

 { "ss.input.mouse_sensitivity", MDFNSF_NOFLAGS, "Emulated mouse sensitivity.", NULL, MDFNST_FLOAT, "0.50", NULL, NULL },

 { "ss.smpc.autortc", MDFNSF_NOFLAGS, "Automatically set RTC on game load.", "Automatically set the SMPC's emulated Real-Time Clock to the host system's current time and date upon game load.", MDFNST_BOOL, "1" },
 { "ss.smpc.autortc.lang", MDFNSF_NOFLAGS, "BIOS language.", NULL, MDFNST_ENUM, "english", NULL, NULL, NULL, NULL, RTCLang_List },

 { "ss.cart", MDFNSF_EMU_STATE | MDFNSF_UNTRUSTED_SAFE, "Expansion cart.", NULL, MDFNST_ENUM, "auto", NULL, NULL, NULL, NULL, Cart_List },
 { "ss.cart.kof95_path", MDFNSF_EMU_STATE, "Path to KoF 95 ROM image.", NULL, MDFNST_STRING, "mpr-18811-mx.ic1" },
 { "ss.cart.ultraman_path", MDFNSF_EMU_STATE, "Path to Ultraman ROM image.", NULL, MDFNST_STRING, "mpr-19367-mx.ic1" },
 
 { "ss.bios_sanity", MDFNSF_NOFLAGS, "Enable BIOS ROM image sanity checks.", NULL, MDFNST_BOOL, "1" },

 { "ss.cd_sanity", MDFNSF_NOFLAGS, "Enable CD (image) sanity checks.", NULL, MDFNST_BOOL, "1" },

 { "ss.slstart", MDFNSF_NOFLAGS, "First displayed scanline in NTSC mode.", NULL, MDFNST_INT, "0", "0", "239" },
 { "ss.slend", MDFNSF_NOFLAGS, "Last displayed scanline in NTSC mode.", NULL, MDFNST_INT, "239", "0", "239" },

 { "ss.slstartp", MDFNSF_NOFLAGS, "First displayed scanline in PAL mode.", NULL, MDFNST_INT, "0", "0", "255" },
 { "ss.slendp", MDFNSF_NOFLAGS, "Last displayed scanline in PAL mode.", NULL, MDFNST_INT, "255", "0", "255" },

 { "ss.midsync", MDFNSF_NOFLAGS, "Enable mid-frame synchronization.", "Mid-frame synchronization can reduce input latency, but it will increase CPU requirements.", MDFNST_BOOL, "0" },

 { "ss.dbg_mask", MDFNSF_NOFLAGS, "Debug printf mask.", NULL, MDFNST_UINT, "0x00001", "0x00000", "0xFFFFF" },
 { "ss.dbg_exe_cdpath", MDFNSF_SUPPRESS_DOC, "CD image to use with homebrew executable loading.", NULL, MDFNST_STRING, "" },

 { NULL },
};

static const CheatInfoStruct CheatInfo =
{
 NULL,
 NULL,

 CheatMemRead,
 CheatMemWrite,

 CheatFormatInfo_Empty,

 true
};

MDFNGI EmulatedSS =
{
 SSSettings,
 0,
 0,

 true, // Multires possible?

 //
 // Note: Following video settings will be overwritten during game load.
 //
 320,	// lcm_width
 240,	// lcm_height
 NULL,  // Dummy

 302,   // Nominal width
 240,   // Nominal height

 0,   // Framebuffer width
 0,   // Framebuffer height
 //
 //
 //

 2,     // Number of output sound channels
};


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
static bool overscan;
static double last_sound_rate;


#define RETRO_DEVICE_SS_PAD       RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0)
#define RETRO_DEVICE_SS_3D_PAD    RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 0)
#define RETRO_DEVICE_SS_MOUSE     RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_MOUSE,  0)

#ifdef NEED_DEINTERLACER
static bool PrevInterlaced;
static Deinterlacer deint;
#endif

static MDFN_Surface *surf = NULL;

static void alloc_surface() {
  MDFN_PixelFormat pix_fmt(MDFN_COLORSPACE_RGB, 16, 8, 0, 24);
  uint32_t width  = MEDNAFEN_CORE_GEOMETRY_MAX_W;
  uint32_t height = MEDNAFEN_CORE_GEOMETRY_MAX_H;

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
#if 0
   // PSX global. Hacky.
   return CD_SelectedDisc;
#else
   return 0;
#endif
}

static bool disk_set_image_index(unsigned index)
{
#if 0
   CD_SelectedDisc = index;
   if (CD_SelectedDisc > disk_get_num_images())
      CD_SelectedDisc = disk_get_num_images();

   // Very hacky. CDSelect command will increment first.
   CD_SelectedDisc--;

   DoSimpleCommand(MDFN_MSC_SELECT_DISK);
   return true;
#else
   return false;
#endif
}

#if 0
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
#endif

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

      layout_md5.starts();

      for(unsigned i = 0; i < CDInterfaces.size(); i++)
      {
         CD_TOC toc;

         TOC_Clear(&toc);
         CDInterfaces[i]->ReadTOC(&toc);

         layout_md5.update_u32_as_lsb(toc.first_track);
         layout_md5.update_u32_as_lsb(toc.last_track);
         layout_md5.update_u32_as_lsb(toc.tracks[100].lba);

         for(uint32 track = toc.first_track; track <= toc.last_track; track++)
         {
            layout_md5.update_u32_as_lsb(toc.tracks[track].lba);
            layout_md5.update_u32_as_lsb(toc.tracks[track].control & 0x4);
         }
      }

      layout_md5.finish(LayoutMD5);
   }

   // TODO: include module name in hash
   memcpy(MDFNGameInfo->MD5, LayoutMD5, 16);

   if(!(LoadCD(&CDInterfaces)))
   {
      for(unsigned i = 0; i < CDInterfaces.size(); i++)
         delete CDInterfaces[i];
      CDInterfaces.clear();

      Cleanup();
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

   if(Load(GameFile) <= 0)
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

   //SMPC_SetInput(0, "gamepad", &input_buf[0]);
   //SMPC_SetInput(1, "gamepad", &input_buf[1]);

   for (unsigned i = 0; i < players; i++)
      SMPC_SetInput(i, "gamepad", (uint8*)&input_buf[i]);
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
      RETRO_DEVICE_ID_JOYPAD_R,
      RETRO_DEVICE_ID_JOYPAD_L,
      RETRO_DEVICE_ID_JOYPAD_X,
      RETRO_DEVICE_ID_JOYPAD_R2,
      RETRO_DEVICE_ID_JOYPAD_UP,
      RETRO_DEVICE_ID_JOYPAD_DOWN,
      RETRO_DEVICE_ID_JOYPAD_LEFT,
      RETRO_DEVICE_ID_JOYPAD_RIGHT,
      RETRO_DEVICE_ID_JOYPAD_B,
      RETRO_DEVICE_ID_JOYPAD_A,
      RETRO_DEVICE_ID_JOYPAD_Y,
      RETRO_DEVICE_ID_JOYPAD_START
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

   static int16_t sound_buf[0x10000];
   EmulateSpecStruct spec = {0};
   spec.surface = surf;
   spec.SoundRate = 44100;
   spec.SoundBuf = sound_buf;
   spec.LineWidths = rects;
   spec.SoundBufMaxSize = sizeof(sound_buf) / 2;
   spec.SoundVolume = 1.0;
   spec.soundmultiplier = 1.0;
   spec.SoundBufSize = 0;
   spec.VideoFormatChanged = false;
   spec.SoundFormatChanged = false;

   EmulateSpecStruct *espec = (EmulateSpecStruct*)&spec;

   if (spec.SoundRate != last_sound_rate)
   {
      spec.SoundFormatChanged = true;
      last_sound_rate = spec.SoundRate;
   }

   Emulate(espec);

   unsigned width        = rects[0];
   unsigned height       = spec.DisplayRect.h;

   video_frames++;
   audio_frames += spec.SoundBufSize;

   video_cb(surf->pixels + surf->pitch32 * spec.DisplayRect.y, width, height, height * 4);
   audio_batch_cb(espec->SoundBuf, spec.SoundBufSize);
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
      case RETRO_DEVICE_SS_PAD:
         log_cb(RETRO_LOG_INFO, "[%s]: Selected controller type standard gamepad.\n", MEDNAFEN_CORE_NAME);
         SMPC_SetInput(in_port, "gamepad", (uint8*)&buf.u8[in_port]);
         break;
      case RETRO_DEVICE_SS_3D_PAD:
         log_cb(RETRO_LOG_INFO, "[%s]: Selected controller type 3D Pad.\n", MEDNAFEN_CORE_NAME);
         SMPC_SetInput(in_port, "3dpad", (uint8*)&buf.u8[in_port]);
         break;
      case RETRO_DEVICE_SS_MOUSE:
         log_cb(RETRO_LOG_INFO, "[%s]: Selected controller type Mouse.\n", MEDNAFEN_CORE_NAME);
         SMPC_SetInput(in_port, "mouse", (uint8*)&buf.u8[in_port]);
         break;
      default:
         log_cb(RETRO_LOG_WARN, "[%s]: Unsupported controller device %u, falling back to gamepad.\n", MEDNAFEN_CORE_NAME,device);
   }

#if 0
   if (rumble.set_rumble_state)
   {
      rumble.set_rumble_state(in_port, RETRO_RUMBLE_STRONG, 0);
      rumble.set_rumble_state(in_port, RETRO_RUMBLE_WEAK, 0);
      buf.u32[in_port][9] = 0;
   }
#endif
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
      { "Saturn Joypad", RETRO_DEVICE_JOYPAD },
      { "3D Pad", RETRO_DEVICE_SS_3D_PAD },
      { "Mouse", RETRO_DEVICE_SS_MOUSE },
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
