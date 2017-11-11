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
#include "input.h"

#include <mednafen/cdrom/cdromif.h>
#include <mednafen/FileStream.h>
#include <mednafen/hash/sha256.h>
#include "mednafen/hash/md5.h"
#include "mednafen/ss/ss.h"
#include "mednafen/ss/debug.inc"

#include <ctype.h>
#include <time.h>

#include <bitset>

#include <zlib.h>

#define MEDNAFEN_CORE_NAME_MODULE            "ss"
#define MEDNAFEN_CORE_NAME                   "Mednafen Saturn"
#define MEDNAFEN_CORE_VERSION                "v0.9.48"
#define MEDNAFEN_CORE_EXTENSIONS             "cue|ccd|chd"
#define MEDNAFEN_CORE_TIMING_FPS             59.82
#define MEDNAFEN_CORE_GEOMETRY_BASE_W        320
#define MEDNAFEN_CORE_GEOMETRY_BASE_H        240
#define MEDNAFEN_CORE_GEOMETRY_MAX_W         704
#define MEDNAFEN_CORE_GEOMETRY_MAX_H         576
#define MEDNAFEN_CORE_GEOMETRY_ASPECT_RATIO  (4.0 / 3.0)
#define FB_WIDTH                             MEDNAFEN_CORE_GEOMETRY_MAX_W

struct retro_perf_callback perf_cb;
retro_get_cpu_features_t perf_get_cpu_features_cb = NULL;
retro_log_printf_t log_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_environment_t environ_cb;

static unsigned frame_count = 0;
static unsigned internal_frame_count = 0;
static bool failed_init = false;
static unsigned image_offset = 0;
static unsigned image_crop = 0;

static unsigned h_mask = 0;
static unsigned first_sl = 0;
static unsigned last_sl = 239;
static unsigned first_sl_pal = 0;
static unsigned last_sl_pal = 287;
static bool is_pal = false;

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
MDFNGI *MDFNGameInfo = NULL;

#include "../MemoryStream.h"

#include "mednafen/ss/ss.h"
#include "mednafen/ss/sound.h"
#include "mednafen/ss/scsp.h"
#include "mednafen/ss/smpc.h"
#include "mednafen/ss/cdb.h"
#include "mednafen/ss/vdp1.h"
#include "mednafen/ss/vdp2.h"
#include "mednafen/ss/scu.h"
#include "mednafen/ss/cart.h"
#include "mednafen/ss/db.h"


static sscpu_timestamp_t MidSync(const sscpu_timestamp_t timestamp);

#ifdef MDFN_SS_DEV_BUILD
uint32 ss_dbg_mask;
#endif
static bool NeedEmuICache;
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

static void CheckEventsByMemTS(void);

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
static uint32 SH7095_DB;

#include "mednafen/ss/scu.inc"
#ifdef HAVE_DEBUG
#include "mednafen/ss/debug.inc"
#endif

static sha256_digest BIOS_SHA256;	// SHA-256 hash of the currently-loaded BIOS; used for save state sanity checks.
static std::vector<CDIF*> *cdifs = NULL;
static std::bitset<1U << (27 - SH7095_EXT_MAP_GRAN_BITS)> FMIsWriteable;

template<typename T>
static void INLINE SH7095_BusWrite(uint32 A, T V, const bool BurstHax, int32* SH2DMAHax);

template<typename T>
static T INLINE SH7095_BusRead(uint32 A, const bool BurstHax, int32* SH2DMAHax);

/*
 SH-2 external bus address map:
  CS0: 0x00000000...0x01FFFFFF (16-bit)
	0x00000000...0x000FFFFF: BIOS ROM (R)
	0x00100000...0x0017FFFF: SMPC (R/W; 8-bit mapped as 16-bit)
	0x00180000...0x001FFFFF: Backup RAM(32KiB) (R/W; 8-bit mapped as 16-bit)
	0x00200000...0x003FFFFF: Low RAM(1MiB) (R/W)
	0x01000000...0x017FFFFF: Slave FRT Input Capture Trigger (W)
	0x01800000...0x01FFFFFF: Master FRT Input Capture Trigger (W)

  CS1: 0x02000000...0x03FFFFFF (SCU managed)
	0x02000000...0x03FFFFFF: A-bus CS0 (R/W)

  CS2: 0x04000000...0x05FFFFFF (SCU managed)
	0x04000000...0x04FFFFFF: A-bus CS1 (R/W)
	0x05000000...0x057FFFFF: A-bus Dummy
	0x05800000...0x058FFFFF: A-bus CS2 (R/W)
	0x05A00000...0x05AFFFFF: SCSP RAM (R/W)
	0x05B00000...0x05BFFFFF: SCSP Registers (R/W)
	0x05C00000...0x05C7FFFF: VDP1 VRAM (R/W)
	0x05C80000...0x05CFFFFF: VDP1 FB RAM (R/W; swappable between two framebuffers, but may be temporarily unreadable at swap time)
	0x05D00000...0x05D7FFFF: VDP1 Registers (R/W)
	0x05E00000...0x05EFFFFF: VDP2 VRAM (R/W)
	0x05F00000...0x05F7FFFF: VDP2 CRAM (R/W; 8-bit writes are illegal)
	0x05F80000...0x05FBFFFF: VDP2 Registers (R/W; 8-bit writes are illegal)
	0x05FE0000...0x05FEFFFF: SCU Registers (R/W)
	0x05FF0000...0x05FFFFFF: SCU Debug/Test Registers (R/W)

  CS3: 0x06000000...0x07FFFFFF
	0x06000000...0x07FFFFFF: High RAM/SDRAM(1MiB) (R/W)
*/
//
// Never add anything to SH7095_mem_timestamp when DMAHax is true.
//
// When BurstHax is true and we're accessing high work RAM, don't add anything.
//
template<typename T, bool IsWrite>
static INLINE void BusRW_DB_CS0(const uint32 A, uint32& DB, const bool BurstHax, int32* SH2DMAHax)
{
 //
 // Low(and kinda slow) work RAM
 //
 if(A >= 0x00200000 && A <= 0x003FFFFF)
 {
  if(IsWrite)
   ne16_wbo_be<T>(WorkRAML, A & 0xFFFFF, DB >> (((A & 1) ^ (2 - sizeof(T))) << 3));
  else
   DB = (DB & 0xFFFF0000) | ne16_rbo_be<uint16>(WorkRAML, A & 0xFFFFE);

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
   DB = (DB & 0xFFFF0000) | ne16_rbo_be<uint16>(BIOSROM, A & 0x7FFFE);

  return;
 }

 //
 // SMPC
 //
 if(A >= 0x00100000 && A <= 0x0017FFFF)
 {
  const uint32 SMPC_A = (A & 0x7F) >> 1;

  if(!SH2DMAHax)
  {
   // SH7095_mem_timestamp += 2;
   CheckEventsByMemTS();
  }

  if(IsWrite)
  {
   if(sizeof(T) == 2 || (A & 1))
    SMPC_Write(SH7095_mem_timestamp, SMPC_A, DB);
  }
  else
   DB = (DB & 0xFFFF0000) | 0xFF00 | SMPC_Read(SH7095_mem_timestamp, SMPC_A);

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
    BackupRAM[(A >> 1) & 0x7FFF] = DB;
    BackupRAM_Dirty = true;
   }
  }
  else
   DB = (DB & 0xFFFF0000) | 0xFF00 | BackupRAM[(A >> 1) & 0x7FFF];

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

  //printf("FT FRT%08x %zu %08x %04x %d %d\n", A, sizeof(T), A, V, SMPC_IsSlaveOn(), SH7095_mem_timestamp);
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
  SS_DBG(SS_DBG_WARNING, "[SH2 BUS] Unknown %zu-byte write of 0x%08x to 0x%08x\n", sizeof(T), DB >> (((A & 1) ^ (2 - sizeof(T))) << 3), A);
 else
  SS_DBG(SS_DBG_WARNING, "[SH2 BUS] Unknown %zu-byte read from 0x%08x\n", sizeof(T), A);
}

template<typename T, bool IsWrite>
static INLINE void BusRW_DB_CS123(const uint32 A, uint32& DB, const bool BurstHax, int32* SH2DMAHax)
{
 //
 // CS3: High work RAM/SDRAM, 0x06000000 ... 0x07FFFFFF
 //
 if(A >= 0x06000000)
 {
  if(!IsWrite || sizeof(T) == 4)
   ne16_rwbo_be<uint32, IsWrite>(WorkRAMH, A & 0xFFFFC, &DB);
  else
   ne16_wbo_be<T>(WorkRAMH, A & 0xFFFFF, DB >> (((A & 3) ^ (4 - sizeof(T))) << 3));

  if(!BurstHax)
  {
   if(!SH2DMAHax)
   {
    if(IsWrite)
    {
     SH7095_mem_timestamp = (SH7095_mem_timestamp + 4) &~ 3;
    }
    else
    {
     SH7095_mem_timestamp += 7;
    }
   }
   else
    *SH2DMAHax -= IsWrite ? 3 : 6;
  }
  return;
 }

 //
 // CS1 and CS2: SCU
 //
 if(!IsWrite)
  DB = 0;

 SCU_FromSH2_BusRW_DB<T, IsWrite>(A, &DB, SH2DMAHax);
}

template<typename T>
static void INLINE SH7095_BusWrite(uint32 A, T V, const bool BurstHax, int32* SH2DMAHax)
{
 uint32 DB = SH7095_DB;

 if(A < 0x02000000)	// CS0, configured as 16-bit
 {
  if(sizeof(T) == 4)
  {
   // TODO/FIXME: Don't allow DMA transfers to occur between the two 16-bit accesses.
   //if(!SH2DMAHax)
   // SH7095_BusLock++;

   DB = (DB & 0xFFFF0000) | (V >> 16);
   BusRW_DB_CS0<uint16, true>(A, DB, BurstHax, SH2DMAHax);

   DB = (DB & 0xFFFF0000) | (uint16)V;
   BusRW_DB_CS0<uint16, true>(A | 2, DB, BurstHax, SH2DMAHax);

   //if(!SH2DMAHax)
   // SH7095_BusLock--;
  }
  else
  {
   const uint32 shift = ((A & 1) ^ (2 - sizeof(T))) << 3;
   const uint32 mask = (0xFFFF >> ((2 - sizeof(T)) * 8)) << shift;

   DB = (DB & ~mask) | (V << shift);
   BusRW_DB_CS0<T, true>(A, DB, BurstHax, SH2DMAHax);
  }
 }
 else	// CS1, CS2, CS3; 32-bit
 {
  const uint32 shift = ((A & 3) ^ (4 - sizeof(T))) << 3;
  const uint32 mask = (0xFFFFFFFF >> ((4 - sizeof(T)) * 8)) << shift;

  DB = (DB & ~mask) | (V << shift); // //ne32_wbo_be<T>(&DB, A & 0x3, V);
  BusRW_DB_CS123<T, true>(A, DB, BurstHax, SH2DMAHax);
 }

 SH7095_DB = DB;
}

template<typename T>
static T INLINE SH7095_BusRead(uint32 A, const bool BurstHax, int32* SH2DMAHax)
{
 uint32 DB = SH7095_DB;
 T ret;

 if(A < 0x02000000)	// CS0, configured as 16-bit
 {
  if(sizeof(T) == 4)
  {
   // TODO/FIXME: Don't allow DMA transfers to occur between the two 16-bit accesses.
   //if(!SH2DMAHax)
   // SH7095_BusLock++;

   BusRW_DB_CS0<uint16, false>(A, DB, BurstHax, SH2DMAHax);
   ret = DB << 16;

   BusRW_DB_CS0<uint16, false>(A | 2, DB, BurstHax, SH2DMAHax);
   ret |= (uint16)DB;

   //if(!SH2DMAHax)
   // SH7095_BusLock--;
  }
  else
  {
   BusRW_DB_CS0<T, false>(A, DB, BurstHax, SH2DMAHax);
   ret = DB >> (((A & 1) ^ (2 - sizeof(T))) << 3);
  }
 }
 else	// CS1, CS2, CS3; 32-bit
 {
  BusRW_DB_CS123<T, false>(A, DB, BurstHax, SH2DMAHax);
  ret = DB >> (((A & 3) ^ (4 - sizeof(T))) << 3);

  // SDRAM leaves data bus in a weird state after read...
  //if(A >= 0x06000000)
  // DB = 0;
 }

 SH7095_DB = DB;
 return ret;
}

//
//
//
static MDFN_COLD uint8 CheatMemRead(uint32 A)
{
 A &= (1U << 27) - 1;

 return ne16_rbo_be<uint8>(SH7095_FastMap[A >> SH7095_EXT_MAP_GRAN_BITS], A);
}

static MDFN_COLD void CheatMemWrite(uint32 A, uint8 V)
{
 A &= (1U << 27) - 1;

 if(FMIsWriteable[A >> SH7095_EXT_MAP_GRAN_BITS])
 {
  ne16_wbo_be<uint8>(SH7095_FastMap[A >> SH7095_EXT_MAP_GRAN_BITS], A, V);

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
event_list_entry events[SS_EVENT__COUNT];

static sscpu_timestamp_t next_event_ts;

template<unsigned c>
static sscpu_timestamp_t SH_DMA_EventHandler(sscpu_timestamp_t et)
{
 if(et < SH7095_mem_timestamp)
 {
  //printf("SH-2 DMA %d reschedule %d->%d\n", c, et, SH7095_mem_timestamp);
  return SH7095_mem_timestamp;
 }

 // Must come after the (et < SH7095_mem_timestamp) check.
 if(MDFN_UNLIKELY(SH7095_BusLock))
  return et + 1;

 return CPU[c].DMA_Update(et);
}

//
//
//

static MDFN_COLD void InitEvents(void)
{
 for(unsigned i = 0; i < SS_EVENT__COUNT; i++)
 {
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

 events[SS_EVENT_CART].event_handler = CART_GetEventHandler();

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

void SS_SetEventNT(event_list_entry* e, const sscpu_timestamp_t next_timestamp)
{
 if(next_timestamp < e->event_time)
 {
  event_list_entry *fe = e;

  do
  {
   fe = fe->prev;
  } while(next_timestamp < fe->event_time);

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
   SS_SetEventNT(&events[evnum], events[evnum].event_handler(timestamp));
 }

 next_event_ts = (Running ? events[SS_EVENT__SYNFIRST].next->event_time : 0);
}

static INLINE bool EventHandler(const sscpu_timestamp_t timestamp)
{
 event_list_entry *e = NULL;

 while(timestamp >= (e = events[SS_EVENT__SYNFIRST].next)->event_time)	// If Running = 0, EventHandler() may be called even if there isn't an event per-se, so while() instead of do { ... } while
 {
#ifdef MDFN_SS_DEV_BUILD
  const sscpu_timestamp_t etime = e->event_time;
#endif
  sscpu_timestamp_t nt;
  nt = e->event_handler(e->event_time);

#ifdef MDFN_SS_DEV_BUILD
  if(MDFN_UNLIKELY(nt <= etime))
  {
   fprintf(stderr, "which=%d event_time=%d nt=%d timestamp=%d\n", (int)(e - events), etime, nt, timestamp);
   assert(nt > etime);
  }
#endif

  SS_SetEventNT(e, nt);
 }

 return(Running);
}

static void CheckEventsByMemTS_Sub(void)
{
 EventHandler(SH7095_mem_timestamp);
}

static void CheckEventsByMemTS(void)
{
 if(MDFN_UNLIKELY(SH7095_mem_timestamp >= next_event_ts))
 {
  //puts("Woot");
  CheckEventsByMemTS_Sub();
 }
}


void SS_RequestMLExit(void)
{
 Running = 0;
 next_event_ts = 0;
}

#pragma GCC push_options
#if !defined(__clang__) && defined(__GNUC__) && __GNUC__ < 5
 // gcc 5.3.0 and 6.1.0 produce some braindead code for the big switch() statement at -Os.
 #pragma GCC optimize("Os,no-unroll-loops,no-peel-loops,no-crossjumping")
#else
 #pragma GCC optimize("O2,no-unroll-loops,no-peel-loops,no-crossjumping")
#endif
template<bool EmulateICache, bool DebugMode>
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

   CPU[0].Step<0, EmulateICache, DebugMode>();
   CPU[0].DMA_BusTimingKludge();

   while(MDFN_LIKELY(CPU[0].timestamp > CPU[1].timestamp))
   {
#ifdef HAVE_DEBUG
    if(DebugMode)
     DBG_CPUHandler<1>(eff_ts);
#endif

    CPU[1].Step<1, EmulateICache, DebugMode>();
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

// Must not be called within an event or read/write handler.
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
static bool AllowMidSync;
static int32 cur_clock_div;

static int64 UpdateInputLastBigTS;
static INLINE void UpdateSMPCInput(const sscpu_timestamp_t timestamp)
{
 int32 elapsed_time = (((int64)timestamp * cur_clock_div * 1000 * 1000) - UpdateInputLastBigTS) / (EmulatedSS.MasterClock / MDFN_MASTERCLOCK_FIXED(1));

 UpdateInputLastBigTS += (int64)elapsed_time * (EmulatedSS.MasterClock / MDFN_MASTERCLOCK_FIXED(1));

 SMPC_UpdateInput(elapsed_time);
}

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
       espec->SoundBufSize += SOUND_FlushOutput();
       espec->MasterCycles = timestamp * cur_clock_div;
    }
    //printf("%d\n", espec->SoundBufSize);

    SMPC_UpdateOutput();
    //
    //
    //MDFN_MidSync(espec);
    //
    //
    UpdateSMPCInput(timestamp);

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

 cur_clock_div = SMPC_StartFrame(espec);
 UpdateSMPCInput(0);
 VDP2::StartFrame(espec, cur_clock_div == 61);
 CART_SetCPUClock(EmulatedSS.MasterClock / MDFN_MASTERCLOCK_FIXED(1), cur_clock_div);
 espec->SoundBufSize = 0;
 espec->MasterCycles = 0;
 espec->soundmultiplier = 1;
 //
 //
 //
 Running = true;	// Set before ForceEventUpdates()
 ForceEventUpdates(0);

#ifdef WANT_DEBUGGER
 #define RLTDAT true
#else
 #define RLTDAT false
#endif
 static int32 (*const rltab[2][2])(EmulateSpecStruct*) =
 {
  //     DebugMode=false        DebugMode=true
  { RunLoop<false, false>, RunLoop<false, RLTDAT> },	// EmulateICache=false
  { RunLoop<true,  false>, RunLoop<true,  RLTDAT> },	// EmulateICache=true
 };
#undef RLTDAT
 end_ts = rltab[NeedEmuICache][DBG_NeedCPUHooks()](espec);

 ForceEventUpdates(end_ts);
 //
 SMPC_EndFrame(espec, end_ts);
 //
 //
 //
 RebaseTS(end_ts);

 CDB_ResetTS();
 SOUND_ResetTS();
 VDP1::AdjustTS(-end_ts);
 VDP2::AdjustTS(-end_ts);
 SMPC_ResetTS();
 SCU_AdjustTS(-end_ts);
 CART_AdjustTS(-end_ts);

 UpdateInputLastBigTS -= (int64)end_ts * cur_clock_div * 1000 * 1000;

 if(!(SH7095_mem_timestamp & 0x40000000))	// or maybe >= 0 instead?
  SH7095_mem_timestamp -= end_ts;

 CPU[0].AdjustTS(-end_ts);

 if(SMPC_IsSlaveOn())
  CPU[1].AdjustTS(-end_ts);
 //
 //
 //
 espec->MasterCycles = end_ts * cur_clock_div;
 espec->SoundBufSize += SOUND_FlushOutput();
 espec->NeedSoundReverse = false;

 SMPC_UpdateOutput();
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

#ifdef HAVE_DEBUG
 DBG_Kill();
#endif
 VDP1::Kill();
 VDP2::Kill();
 SOUND_Kill();
 CDB_Kill();

 cdifs = NULL;
}

static MDFN_COLD bool IsSaturnDisc(const uint8* sa32k)
{
 if(sha256(&sa32k[0x100], 0xD00) != "96b8ea48819cfa589f24c40aa149c224c420dccf38b730f00156efe25c9bbc8f"_sha256)
  return false;

 if(memcmp(&sa32k[0], "SEGA SEGASATURN ", 16))
  return false;

 log_cb(RETRO_LOG_INFO, "[Mednafen]: Is a Saturn disc.\n");

 return true;
}

static INLINE void CalcGameID(uint8* id_out16, uint8* fd_id_out16, char* sgid)
{
   md5_context mctx;
   uint8_t buf[2048];

   log_cb(RETRO_LOG_INFO, "Start calculating game ID, discs: %d...\n", cdifs ? cdifs->size() : 0);

   mctx.starts();

   for(size_t x = 0; x < cdifs->size(); x++)
   {
      CDIF *c = (*cdifs)[x];
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
               memcpy(sgid, (void*)(&buf[0x20]), 16);
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


static INLINE bool DetectRegion(unsigned* const region)
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
     log_cb(RETRO_LOG_INFO, "[Mednafen]: Found region: %d\n", (uint64)1 << rs.region);
   *region = rs.region;
   return true;
  }
 }

 return false;
}

static MDFN_COLD bool DetectRegionByFN(const std::string& fn, unsigned* const region)
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

typedef struct
{
   const unsigned type;
   const char *name;
} CartName;

static bool InitCommon(const unsigned cpucache_emumode, const unsigned cart_type, const unsigned smpc_area)
{
#ifdef MDFN_SS_DEV_BUILD
 ss_dbg_mask = SS_DBG_ERROR;
 {
  std::vector<uint64> dms = MDFN_GetSettingMultiUI("ss.dbg_mask");

  for(uint64 dmse : dms)
   ss_dbg_mask |= dmse;
 }
#endif
 //

   unsigned i;
 //
 {
  const struct
  {
   unsigned mode;
   const char* name;
  } CPUCacheEmuModes[] =
  {
   { CPUCACHE_EMUMODE_DATA_CB,	_("Data only, with high-level bypass") },
   { CPUCACHE_EMUMODE_DATA,	_("Data only") },
   { CPUCACHE_EMUMODE_FULL,	_("Full") },
  };
  const char* cem = _("Unknown");

  for(auto const& ceme : CPUCacheEmuModes)
  {
   if(ceme.mode == cpucache_emumode)
   {
    cem = ceme.name;
    break;
   }
  }
  log_cb(RETRO_LOG_INFO, "[Mednafen]: CPU Cache Emulation Mode: %s\n", cem);
 }
 //
   {
      log_cb(RETRO_LOG_INFO, "[Mednafen]: Region: 0x%01x.\n", smpc_area);
      const CartName CartNames[] =
      {
         { CART_NONE, "None" },
         { CART_BACKUP_MEM, "Backup Memory" },
         { CART_EXTRAM_1M, "1MiB Extended RAM" },
         { CART_EXTRAM_4M, "4MiB Extended RAM" },
         { CART_KOF95, "King of Fighters '95 ROM" },
         { CART_ULTRAMAN, "Ultraman ROM" },
         { CART_CS1RAM_16M, _("16MiB CS1 RAM") },
         { CART_NLMODEM, _("Netlink Modem") },
         { CART_MDFN_DEBUG, "Mednafen Debug" }
      };
      const char* cn = "Unknown";

      for(i = 0; i < ARRAY_SIZE(CartNames); i++)
      {
         CartName cne = CartNames[i];
         if(cne.type != cart_type)
            continue;
         cn = cne.name;
         break;
      }
      log_cb(RETRO_LOG_INFO, "[Mednafen]: Cart: %s.\n", cn);
   }
   //

   NeedEmuICache = (cpucache_emumode == CPUCACHE_EMUMODE_FULL);
   for(i = 0; i < 2; i++)
   {
      CPU[i].Init(cpucache_emumode == CPUCACHE_EMUMODE_DATA_CB);
      CPU[i].SetMD5((bool)i);
   }

   //
   // Initialize backup memory.
   //
   memset(BackupRAM, 0x00, sizeof(BackupRAM));
   for(i = 0; i < 0x40; i++)
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
   is_pal = PAL;
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
         log_cb(RETRO_LOG_ERROR, "BIOS file \"%s\" is of an incorrect size.\n", biospath.c_str());
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
                  log_cb(RETRO_LOG_ERROR, "Wrong BIOS for region being emulated.\n");
                  return false;
               }
            }
            else if(fn == dbe.fn)	// Discourage people from renaming files instead of changing settings.
            {
               log_cb(RETRO_LOG_ERROR,
                     "BIOS hash does not match that as expected by filename.\n");
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
   UpdateInputLastBigTS = 0;

#ifdef HAVE_DEBUG
   DBG_Init();
#endif

#if 0
   MDFN_printf("\n");
   {
      const bool correct_aspect = MDFN_GetSettingB("ss.correct_aspect");
      const bool h_overscan = MDFN_GetSettingB("ss.h_overscan");
      const bool h_blend = MDFN_GetSettingB("ss.h_blend");

      MDFN_printf(_("Displayed scanlines: [%u,%u]\n"), sls, sle);
      MDFN_printf(_("Correct Aspect Ratio: %s\n"), correct_aspect ? _("Enabled") : _("Disabled"));
      MDFN_printf(_("Show H Overscan: %s\n"), h_overscan ? _("Enabled") : _("Disabled"));
      MDFN_printf(_("H Blend: %s\n"), h_blend ? _("Enabled") : _("Disabled"));

      VDP2::SetGetVideoParams(&EmulatedSS, correct_aspect, sls, sle, h_overscan, h_blend);
   }

   MDFN_printf("\n");
#endif

   for(unsigned sp = 0; sp < 2; sp++)
   {
      char buf[64];
      bool sv;

      snprintf(buf, sizeof(buf), "ss.input.sport%u.multitap", sp + 1);
      sv = MDFN_GetSettingB(buf);
      SMPC_SetMultitap(sp, sv);

#if 0
      MDFN_printf(_("Multitap on Saturn Port %u: %s\n"), sp + 1, sv ? _("Enabled") : _("Disabled"));
#endif
   }


   for(unsigned vp = 0; vp < 12; vp++)
   {
      char buf[64];
      uint32 sv;

      snprintf(buf, sizeof(buf), "ss.input.port%u.gun_chairs", vp + 1);
      sv = MDFN_GetSettingUI(buf);
      SMPC_SetCrosshairsColor(vp, sv);
   }
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
         log_cb(RETRO_LOG_ERROR,
               "AutoRTC error #1\n");
         return false;
      }

      if((ht = localtime(&ut)) == NULL)
      {
         log_cb(RETRO_LOG_ERROR,
               "AutoRTC error #2\n");
         return false;
      }

      SMPC_SetRTC(ht, MDFN_GetSettingUI("ss.smpc.autortc.lang"));
   }
   //
   SS_Reset(true);

   return true;
}

#ifdef MDFN_SS_DEV_BUILD
static bool TestMagic(MDFNFILE* fp)
{
 return false;
}
#endif

static MDFN_COLD bool Load(MDFNFILE* fp)
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
#ifdef MDFN_SS_DEV_BUILD
   if(MDFN_GetSettingS("ss.dbg_exe_cdpath") != "")
   {
      RMD_Drive dr;
      RMD_DriveDefaults drdef;

      dr.Name = std::string("Virtual CD Drive");
      dr.PossibleStates.push_back(RMD_State({"Tray Open", false, false, true}));
      dr.PossibleStates.push_back(RMD_State({"Tray Closed (Empty)", false, false, false}));
      dr.PossibleStates.push_back(RMD_State({"Tray Closed", true, true, false}));
      dr.CompatibleMedia.push_back(0);
      dr.MediaMtoPDelay = 2000;

      drdef.State = 2; // Tray Closed
      drdef.Media = 0;
      drdef.Orientation = 0;

      MDFNGameInfo->RMD->Drives.push_back(dr);
      MDFNGameInfo->RMD->DrivesDefaults.push_back(drdef);
      MDFNGameInfo->RMD->MediaTypes.push_back(RMD_MediaType({"CD"}));
      MDFNGameInfo->RMD->Media.push_back(RMD_Media({"Test CD", 0}));

      static std::vector<CDIF *> CDInterfaces;
      CDInterfaces.clear();
      CDInterfaces.push_back(CDIF_Open(MDFN_GetSettingS("ss.dbg_exe_cdpath").c_str(), false));
      cdifs = &CDInterfaces;
   }
#endif

   if (!InitCommon(CPUCACHE_EMUMODE_DATA, CART_MDFN_DEBUG, MDFN_GetSettingUI("ss.region_default")))
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
               log_cb(RETRO_LOG_ERROR,
                     "Disc %zu of %zu: Error reading sector at lba=%d in DiscSanityChecks().\n", i + 1, cdifs->size(), lba);
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
                  log_cb(RETRO_LOG_ERROR,
                  "Disc %zu of %zu: Time mismatch at lba=%d(%02x:%02x:%02x); Q subchannel: %02x:%02x:%02x\n",
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
            log_cb(RETRO_LOG_ERROR,
                  "Disc %zu of %zu: No valid Q subchannel ADR_CURPOS data present at lba %d-%d?!\n", i + 1, cdifs->size(), start_lba, end_lba);
            return false;
         }

         break;
      }
   }

   return true;
}

static MDFN_COLD bool LoadCD(std::vector<CDIF *>* CDInterfaces)
{
   const int ss_cart_setting = MDFN_GetSettingI("ss.cart");
   unsigned region;
   int cart_type;
   unsigned cpucache_emumode;
   uint8 fd_id[16];
   char sgid[16 + 1] = { 0 };
   cdifs = CDInterfaces;
   CalcGameID(MDFNGameInfo->MD5, fd_id, sgid);

   log_cb(RETRO_LOG_INFO, "Game ID is: %s\n", sgid);

   region = setting_region;
   cart_type = CART_BACKUP_MEM;
   cpucache_emumode = CPUCACHE_EMUMODE_DATA;

   DetectRegion(&region);
   DB_Lookup(nullptr, sgid, fd_id, &region, &cart_type, &cpucache_emumode);

   // forced region setting?
   if ( setting_region != 0 ) {
	   region = setting_region;
   }

  if(ss_cart_setting != CART__RESERVED)
   cart_type = ss_cart_setting;
  //
   if(MDFN_GetSettingB("ss.cd_sanity"))
   {
      log_cb(RETRO_LOG_INFO, "Trying to do CD sanity checks...\n");
      if (!DiscSanityChecks())
         return false;
   }
   else
   {
      log_cb(RETRO_LOG_WARN,
            "WARNING: CD (image) sanity checks disabled.\n");
    }

   // TODO: auth ID calc


   if (!InitCommon(cpucache_emumode, cart_type, region))
      return false;

   return true;
}

static MDFN_COLD void CloseGame(void)
{
#ifdef MDFN_SS_DEV_BUILD
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

void MDFN_BackupSavFile(const uint8 max_backup_count, const char* sav_ext)
{
	// stub for libretro port
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
 bool nv16 = false;
 uint64 nv_size = 0;

 CART_GetNVInfo(&ext, &nv_ptr, &nv16, &nv_size);

 if(ext)
  MDFN_BackupSavFile(10, ext);
}

static MDFN_COLD void LoadCartNV(void)
{
 const char* ext = nullptr;
 void* nv_ptr = nullptr;
 bool nv16 = false;
 uint64 nv_size = 0;

 CART_GetNVInfo(&ext, &nv_ptr, &nv16, &nv_size);

 if(ext)
 {
  FileStream nvs(MDFN_MakeFName(MDFNMKF_SAV, 0, ext), MODE_READ);

  nvs.read(nv_ptr, nv_size);

  if(nv16)
  {
   for(uint64 i = 0; i < nv_size; i += 2)
   {
    void* p = (uint8*)nv_ptr + i;

    MDFN_ennsb<uint16>(p, MDFN_de16msb(p));
   }
  }
 }
}

static MDFN_COLD void SaveCartNV(void)
{
 const char* ext = nullptr;
 void* nv_ptr = nullptr;
 bool nv16 = false;
 uint64 nv_size = 0;

 CART_GetNVInfo(&ext, &nv_ptr, &nv16, &nv_size);

 if(ext)
 {
  FileStream nvs(MDFN_MakeFName(MDFNMKF_SAV, 0, ext), MODE_WRITE_INPLACE);

  if(nv16)
  {
   // Slow...
   for(uint64 i = 0; i < nv_size; i += 2)
    nvs.put_BE<uint16>(MDFN_densb<uint16>((uint8*)nv_ptr + i));
  }
  else
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

struct EventsPacker
{
 enum : size_t { eventcopy_first = SS_EVENT__SYNFIRST + 1 };
 enum : size_t { eventcopy_bound = SS_EVENT__SYNLAST };

 bool Restore(void);
 void Save(void);

 int32 event_times[eventcopy_bound - eventcopy_first];
 uint8 event_order[eventcopy_bound - eventcopy_first];
};

INLINE void EventsPacker::Save(void)
{
 event_list_entry* evt = events[SS_EVENT__SYNFIRST].next;

 for(size_t i = eventcopy_first; i < eventcopy_bound; i++)
 {
  event_times[i - eventcopy_first] = events[i].event_time;
  event_order[i - eventcopy_first] = evt - events;
  assert(event_order[i - eventcopy_first] >= eventcopy_first && event_order[i - eventcopy_first] < eventcopy_bound);
  evt = evt->next;
 }
}

INLINE bool EventsPacker::Restore(void)
{
 bool used[SS_EVENT__COUNT] = { 0 };
 event_list_entry* evt = &events[SS_EVENT__SYNFIRST];
 for(size_t i = eventcopy_first; i < eventcopy_bound; i++)
 {
  int32 et = event_times[i - eventcopy_first];
  uint8 eo = event_order[i - eventcopy_first];

  if(eo < eventcopy_first || eo >= eventcopy_bound)
   return false;

  if(used[eo])
   return false;

  used[eo] = true;

  if(et < events[SS_EVENT__SYNFIRST].event_time)
   return false;

  events[i].event_time = et;

  evt->next = &events[eo];
  evt->next->prev = evt;
  evt = evt->next;
 }
 evt->next = &events[SS_EVENT__SYNLAST];
 evt->next->prev = evt;

 for(size_t i = 0; i < SS_EVENT__COUNT; i++)
 {
  if(i == SS_EVENT__SYNLAST)
  {
   if(events[i].next != NULL)
    return false;
  }
  else
  {
   if(events[i].next->prev != &events[i])
    return false;

   if(events[i].next->event_time < events[i].event_time)
    return false;
  }

  if(i == SS_EVENT__SYNFIRST)
  {
   if(events[i].prev != NULL)
    return false;
  }
  else
  {
   if(events[i].prev->next != &events[i])
    return false;

   if(events[i].prev->event_time > events[i].event_time)
    return false;
  }
 }

 return true;
}

MDFN_COLD int LibRetro_StateAction( StateMem* sm, const unsigned load, const bool data_only )
{
	int success;

	if ( data_only == false )
	{
		sha256_digest sr_dig = BIOS_SHA256;

		SFORMAT SRDStateRegs[] =
		{
			SFARRAY( sr_dig.data(), sr_dig.size() ),
			SFEND
		};

		success = MDFNSS_StateAction( sm, load, data_only, SRDStateRegs, "BIOS_HASH", true );
		if ( success == 0 ) {
			return 0;
		}

		if ( load && sr_dig != BIOS_SHA256 ) {
			log_cb( RETRO_LOG_WARN, "BIOS hash mismatch(save state created under a different BIOS)!\n" );
			return 0;
		}
	}

 EventsPacker ep;
 ep.Save();

 SFORMAT StateRegs[] =
 {
  // cur_clock_div
  SFVAR(UpdateInputLastBigTS),

  SFVAR(next_event_ts),
  SFARRAY32N(ep.event_times, sizeof(ep.event_times) / sizeof(ep.event_times[0]), "event_times"),
  SFARRAYN(ep.event_order, sizeof(ep.event_order) / sizeof(ep.event_order[0]), "event_order"),

  SFVAR(SH7095_mem_timestamp),
  SFVAR(SH7095_BusLock),
  SFVAR(SH7095_DB),

  SFARRAY16(WorkRAML, sizeof(WorkRAML) / sizeof(WorkRAML[0])),
  SFARRAY16(WorkRAMH, sizeof(WorkRAMH) / sizeof(WorkRAMH[0])),
  SFARRAY(BackupRAM, sizeof(BackupRAM) / sizeof(BackupRAM[0])),

  SFEND
 };

 CPU[0].StateAction(sm, load, data_only, "SH2-M");
 CPU[1].StateAction(sm, load, data_only, "SH2-S");
 SCU_StateAction(sm, load, data_only);
 SMPC_StateAction(sm, load, data_only);

 CDB_StateAction(sm, load, data_only);
 VDP1::StateAction(sm, load, data_only);
 VDP2::StateAction(sm, load, data_only);

 SOUND_StateAction(sm, load, data_only);
 CART_StateAction(sm, load, data_only);
 //
	success = MDFNSS_StateAction(sm, load, data_only, StateRegs, "MAIN");
	if ( success == 0 ) {
		log_cb( RETRO_LOG_ERROR, "Failed to load MAIN state objects.\n" );
		return 0;
	}

	if ( load )
	{
		BackupRAM_Dirty = true;

		if ( !ep.Restore() )
		{
			log_cb( RETRO_LOG_WARN, "Bad state events data.\n" );
			InitEvents();
		}
	}

	// Success!
	return 1;
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
 { "cs1ram16", CART_CS1RAM_16M, "16MiB RAM mapped in A-bus CS1" },
 { "ar4mp", CART_AR4MP, NULL }, // Undocumented, unfinished. "Action Replay 4M Plus" },
 // { "nlmodem", CART_NLMODEM, "NetLink Modem" },

 { NULL, 0 },
};

static MDFNSetting SSSettings[] =
{
   { "ss.bios_jp", MDFNSF_EMU_STATE, "Path to the Japan ROM BIOS", NULL, MDFNST_STRING, "sega_101.bin" },
   { "ss.bios_na_eu", MDFNSF_EMU_STATE, "Path to the North America and Europe ROM BIOS", NULL, MDFNST_STRING, "mpr-17933.bin" },

 { "ss.scsp.resamp_quality", MDFNSF_NOFLAGS, "SCSP output resampler quality.",
	"0 is lowest quality and CPU usage, 10 is highest quality and CPU usage.  The resampler that this setting refers to is used for converting from 44.1KHz to the sampling rate of the host audio device Mednafen is using.  Changing Mednafen's output rate, via the \"sound.rate\" setting, to \"44100\" may bypass the resampler, which can decrease CPU usage by Mednafen, and can increase or decrease audio quality, depending on various operating system and hardware factors.", MDFNST_UINT, "4", "0", "10" },

 { "ss.input.mouse_sensitivity", MDFNSF_NOFLAGS, "Emulated mouse sensitivity.", NULL, MDFNST_FLOAT, "0.50", NULL, NULL },
  { "ss.input.sport1.multitap", MDFNSF_EMU_STATE | MDFNSF_UNTRUSTED_SAFE, "Enable multitap on Saturn port 1.", NULL, MDFNST_BOOL, "0", NULL, NULL },
 { "ss.input.sport2.multitap", MDFNSF_EMU_STATE | MDFNSF_UNTRUSTED_SAFE, "Enable multitap on Saturn port 2.", NULL, MDFNST_BOOL, "0", NULL, NULL },

 { "ss.input.port1.gun_chairs",  MDFNSF_NOFLAGS, "Crosshairs color for lightgun on virtual port 1.",  "A value of 0x1000000 disables crosshair drawing.", MDFNST_UINT, "0xFF0000", "0x000000", "0x1000000" },
 { "ss.input.port2.gun_chairs",  MDFNSF_NOFLAGS, "Crosshairs color for lightgun on virtual port 2.",  "A value of 0x1000000 disables crosshair drawing.", MDFNST_UINT, "0x00FF00", "0x000000", "0x1000000" },
 { "ss.input.port3.gun_chairs",  MDFNSF_NOFLAGS, "Crosshairs color for lightgun on virtual port 3.",  "A value of 0x1000000 disables crosshair drawing.", MDFNST_UINT, "0xFF00FF", "0x000000", "0x1000000" },
 { "ss.input.port4.gun_chairs",  MDFNSF_NOFLAGS, "Crosshairs color for lightgun on virtual port 4.",  "A value of 0x1000000 disables crosshair drawing.", MDFNST_UINT, "0xFF8000", "0x000000", "0x1000000" },
 { "ss.input.port5.gun_chairs",  MDFNSF_NOFLAGS, "Crosshairs color for lightgun on virtual port 5.",  "A value of 0x1000000 disables crosshair drawing.", MDFNST_UINT, "0xFFFF00", "0x000000", "0x1000000" },
 { "ss.input.port6.gun_chairs",  MDFNSF_NOFLAGS, "Crosshairs color for lightgun on virtual port 6.",  "A value of 0x1000000 disables crosshair drawing.", MDFNST_UINT, "0x00FFFF", "0x000000", "0x1000000" },
 { "ss.input.port7.gun_chairs",  MDFNSF_NOFLAGS, "Crosshairs color for lightgun on virtual port 7.",  "A value of 0x1000000 disables crosshair drawing.", MDFNST_UINT, "0x0080FF", "0x000000", "0x1000000" },
 { "ss.input.port8.gun_chairs",  MDFNSF_NOFLAGS, "Crosshairs color for lightgun on virtual port 8.",  "A value of 0x1000000 disables crosshair drawing.", MDFNST_UINT, "0x8000FF", "0x000000", "0x1000000" },
 { "ss.input.port9.gun_chairs",  MDFNSF_NOFLAGS, "Crosshairs color for lightgun on virtual port 9.",  "A value of 0x1000000 disables crosshair drawing.", MDFNST_UINT, "0xFF80FF", "0x000000", "0x1000000" },
 { "ss.input.port10.gun_chairs", MDFNSF_NOFLAGS, "Crosshairs color for lightgun on virtual port 10.", "A value of 0x1000000 disables crosshair drawing.", MDFNST_UINT, "0x00FF80", "0x000000", "0x1000000" },
 { "ss.input.port11.gun_chairs", MDFNSF_NOFLAGS, "Crosshairs color for lightgun on virtual port 11.", "A value of 0x1000000 disables crosshair drawing.", MDFNST_UINT, "0x8080FF", "0x000000", "0x1000000" },
 { "ss.input.port12.gun_chairs", MDFNSF_NOFLAGS, "Crosshairs color for lightgun on virtual port 12.", "A value of 0x1000000 disables crosshair drawing.", MDFNST_UINT, "0xFF8080", "0x000000", "0x1000000" },



 { "ss.smpc.autortc", MDFNSF_NOFLAGS, "Automatically set RTC on game load.", "Automatically set the SMPC's emulated Real-Time Clock to the host system's current time and date upon game load.", MDFNST_BOOL, "1" },
 { "ss.smpc.autortc.lang", MDFNSF_NOFLAGS, "BIOS language.", NULL, MDFNST_ENUM, "english", NULL, NULL, NULL, NULL, RTCLang_List },

 { "ss.cart", MDFNSF_EMU_STATE | MDFNSF_UNTRUSTED_SAFE, "Expansion cart.", NULL, MDFNST_ENUM, "auto", NULL, NULL, NULL, NULL, Cart_List },
 { "ss.cart.kof95_path", MDFNSF_EMU_STATE, "Path to KoF 95 ROM image.", NULL, MDFNST_STRING, "mpr-18811-mx.ic1" },
 { "ss.cart.ultraman_path", MDFNSF_EMU_STATE, "Path to Ultraman ROM image.", NULL, MDFNST_STRING, "mpr-19367-mx.ic1" },
 { "ss.cart.satar4mp_path", MDFNSF_EMU_STATE | MDFNSF_SUPPRESS_DOC | MDFNSF_NONPERSISTENT, "Path to Action Replay 4M Plus firmware image.", NULL, MDFNST_STRING, "satar4mp.bin" },

 // { "ss.cart.modem_port", MDFNSF_NOFLAGS, "TCP/IP port to use for modem emulation.", "A value of \"0\" disables network access.", MDFNST_UINT, "4920", "0", "65535" },

 { "ss.bios_sanity", MDFNSF_NOFLAGS, "Enable BIOS ROM image sanity checks.", NULL, MDFNST_BOOL, "1" },

 { "ss.cd_sanity", MDFNSF_NOFLAGS, "Enable CD (image) sanity checks.", NULL, MDFNST_BOOL, "1" },

 { "ss.slstart", MDFNSF_NOFLAGS, "First displayed scanline in NTSC mode.", NULL, MDFNST_INT, "0", "0", "239" },
 { "ss.slend", MDFNSF_NOFLAGS, "Last displayed scanline in NTSC mode.", NULL, MDFNST_INT, "239", "0", "239" },

 { "ss.slstartp", MDFNSF_NOFLAGS, "First displayed scanline in PAL mode.", NULL, MDFNST_INT, "0", "-16", "271" },
 { "ss.slendp", MDFNSF_NOFLAGS, "Last displayed scanline in PAL mode.", NULL, MDFNST_INT, "255", "-16", "271" },

 { "ss.midsync", MDFNSF_NOFLAGS, "Enable mid-frame synchronization.", "Mid-frame synchronization can reduce input latency, but it will increase CPU requirements.", MDFNST_BOOL, "0" },

#ifdef MDFN_SS_DEV_BUILD
 { "ss.dbg_mask", MDFNSF_NOFLAGS, "Debug printf mask.", NULL, MDFNST_UINT, "0x00001", "0x00000", "0xFFFFF" },
 { "ss.dbg_exe_cdpath", MDFNSF_SUPPRESS_DOC, "CD image to use with homebrew executable loading.", NULL, MDFNST_STRING, "" },
#endif

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

 320,   // Nominal width
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

   CDUtility_Init();

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

   setting_region = 0; // auto
   setting_smpc_autortc = true;
   setting_smpc_autortc_lang = 0;
   setting_initial_scanline = 0;
   setting_last_scanline = 239;
   setting_initial_scanline_pal = 0;
   setting_last_scanline_pal = 287;

   check_system_specs();
}

void retro_reset(void)
{
#if 0
   DoSimpleCommand(MDFN_MSC_RESET);
#else
   DoSimpleCommand(MDFN_MSC_POWER);
#endif
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

   if (startup)
   {
   }

	var.key = "beetle_saturn_region";

	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
	{
		if (!strcmp(var.value, "Auto Detect") || !strcmp(var.value, "auto"))
			setting_region = 0;
		else if (!strcmp(var.value, "Japan") || !strcmp(var.value, "jp"))
			setting_region = SMPC_AREA_JP;
		else if (!strcmp(var.value, "North America") || !strcmp(var.value, "na"))
			setting_region = SMPC_AREA_NA;
		else if (!strcmp(var.value, "Europe") || !strcmp(var.value, "eu"))
			setting_region = SMPC_AREA_EU_PAL;
		else if (!strcmp(var.value, "South Korea") || !strcmp(var.value, "kr"))
			setting_region = SMPC_AREA_KR;
		else if (!strcmp(var.value, "Asia (NTSC)") || !strcmp(var.value, "tw"))
			setting_region = SMPC_AREA_ASIA_NTSC;
		else if (!strcmp(var.value, "Asia (PAL)") || !strcmp(var.value, "as"))
			setting_region = SMPC_AREA_ASIA_PAL;
		else if (!strcmp(var.value, "Brazil") || !strcmp(var.value, "br"))
			setting_region = SMPC_AREA_CSA_NTSC;
		else if (!strcmp(var.value, "Latin America") || !strcmp(var.value, "la"))
			setting_region = SMPC_AREA_CSA_PAL;
	}

   var.key = "beetle_saturn_cdimagecache";

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

   var.key = "beetle_saturn_autortc";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "enabled") == 0)
         setting_smpc_autortc = 1;
      else if (strcmp(var.value, "disabled") == 0)
         setting_smpc_autortc = 0;
   }

   var.key = "beetle_saturn_autortc_lang";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
       if (strcmp(var.value, "english") == 0)
          setting_smpc_autortc_lang = 0;
       else if (strcmp(var.value, "german") == 0)
          setting_smpc_autortc_lang = 1;
       else if (strcmp(var.value, "french") == 0)
          setting_smpc_autortc_lang = 2;
       else if (strcmp(var.value, "spanish") == 0)
          setting_smpc_autortc_lang = 3;
       else if (strcmp(var.value, "italian") == 0)
          setting_smpc_autortc_lang = 4;
       else if (strcmp(var.value, "japanese") == 0)
          setting_smpc_autortc_lang = 5;
   }

   var.key = "beetle_saturn_horizontal_overscan";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      h_mask = atoi(var.value);
   }

   var.key = "beetle_saturn_initial_scanline";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      first_sl = atoi(var.value);
   }

   var.key = "beetle_saturn_last_scanline";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      last_sl = atoi(var.value);
   }

   var.key = "beetle_saturn_initial_scanline_pal";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      first_sl_pal = atoi(var.value);
   }

   var.key = "beetle_saturn_last_scanline_pal";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      last_sl_pal = atoi(var.value);
   }

	var.key = "beetle_saturn_analog_stick_deadzone";
	var.value = NULL;

	if ( environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value )
		input_set_deadzone_stick( atoi( var.value ) );
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

static std::vector<CDIF *> CDInterfaces;	// FIXME: Cleanup on error out.
// TODO: LoadCommon()

static MDFNGI *MDFNI_LoadCD(const char *devicename)
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
            CDIF *image  = CDIF_Open(file_list[i].c_str(), false);
            CDInterfaces.push_back(image);
         }
      }
      else
      {
         bool success = true;
         CDIF *image  = CDIF_Open(devicename, false);
         log_cb(RETRO_LOG_INFO, "Pushing CD image onto stack: %s.\n", devicename);
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

      CDInterfaces[i]->ReadTOC(&toc);

      log_cb(RETRO_LOG_DEBUG, "CD %d Layout:\n", i + 1);

      for(int32 track = toc.first_track; track <= toc.last_track; track++)
      {
         log_cb(RETRO_LOG_DEBUG, "Track %2d, LBA: %6d  %s\n", track, toc.tracks[track].lba, (toc.tracks[track].control & 0x4) ? "DATA" : "AUDIO");
      }

      log_cb(RETRO_LOG_DEBUG, "Leadout: %6d\n", toc.tracks[100].lba);
   }

   log_cb(RETRO_LOG_DEBUG, "Calculating layout MD5.\n");
   // Calculate layout MD5.  The system emulation LoadCD() code is free to ignore this value and calculate
   // its own, or to use it to look up a game in its database.
   {
      md5_context layout_md5;

      layout_md5.starts();

      for(unsigned i = 0; i < CDInterfaces.size(); i++)
      {
         TOC toc;

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

   log_cb(RETRO_LOG_DEBUG, "Done calculating layout MD5.\n");
   // TODO: include module name in hash
   if (MDFNGameInfo == NULL)
   {
      MDFNGameInfo = &EmulatedSS;
   }

   memcpy(MDFNGameInfo->MD5, LayoutMD5, 16);

   if(!(LoadCD(&CDInterfaces)))
   {
      Cleanup();
      return NULL;
   }

   //MDFNI_SetLayerEnableMask(~0ULL);

   MDFN_LoadGameCheats(NULL);
   MDFNMP_InstallReadPatches();

   return(MDFNGameInfo);
}
#endif

static MDFNGI *MDFNI_LoadGame(const char *name)
{
   MDFNFILE *GameFile = NULL;

	if(strlen(name) > 4 && (!strcasecmp(name + strlen(name) - 4, ".cue") || !strcasecmp(name + strlen(name) - 4, ".ccd") || !strcasecmp(name + strlen(name) - 4, ".chd") || !strcasecmp(name + strlen(name) - 4, ".toc") || !strcasecmp(name + strlen(name) - 4, ".m3u") || !strcasecmp(name + strlen(name) - 4, ".pbp")))
   {
      MDFNGI *gi = MDFNI_LoadCD(name);
      if (!gi)
         goto error;
      return gi;
   }

   GameFile = file_open(name);

   if(!GameFile)
      goto error;

   if(Load(GameFile) <= 0)
      goto error;

   file_close(GameFile);
   GameFile   = NULL;

   return(MDFNGameInfo);

error:
   for(unsigned i = 0; i < CDInterfaces.size(); i++)
      delete CDInterfaces[i];
   CDInterfaces.clear();
   if (GameFile)
      file_close(GameFile);
   GameFile     = NULL;
   MDFNGameInfo = NULL;
   return NULL;
}

bool retro_load_game(const struct retro_game_info *info)
{
   char tocbasepath[4096];
   bool ret = false;

   if (!info || failed_init)
      return false;

	input_init_env( environ_cb );

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

   if (!MDFNI_LoadGame(retro_cd_path))
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

	input_init();

   boot = false;

   cdifs = &CDInterfaces;
   CDB_SetDisc(false, (*cdifs)[0]);

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

   if (MDFNGameInfo->RMD)
   {
      delete MDFNGameInfo->RMD;
      MDFNGameInfo->RMD = NULL;
   }

   MDFNMP_Kill();

   MDFNGameInfo = NULL;

   for(unsigned i = 0; i < CDInterfaces.size(); i++)
      delete CDInterfaces[i];
   CDInterfaces.clear();

   retro_cd_base_directory[0] = '\0';
   retro_cd_path[0]           = '\0';
   retro_cd_base_name[0]      = '\0';
}

void update_geometry(unsigned width, unsigned height)
{
   struct retro_system_av_info system_av_info;
   system_av_info.geometry.base_width = width;
   system_av_info.geometry.base_height = height;
   system_av_info.geometry.aspect_ratio = MEDNAFEN_CORE_GEOMETRY_ASPECT_RATIO;
   environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &system_av_info);
}

static uint64_t video_frames, audio_frames;
#define SOUND_CHANNELS 2

void retro_run(void)
{
   bool updated = false;
   bool resolution_changed = false;
   unsigned linevisfirst, overscan_mask;
   static unsigned width, height, source_height;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
   {
      check_variables(false);
      resolution_changed = true;
   }

   // Keep the counters at 0 so that they don't display a bogus
   // value if this option is enabled later on
   frame_count = 0;
   internal_frame_count = 0;

   input_poll_cb();

   input_update( input_state_cb );

   static int32 rects[FB_WIDTH];
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

#ifdef NEED_DEINTERLACER
   if (spec.InterlaceOn)
   {
      if (!PrevInterlaced)
         deint.ClearState();

      deint.Process(spec.surface, spec.DisplayRect, spec.LineWidths, spec.InterlaceField);

      PrevInterlaced = true;

      spec.InterlaceOn = false;
      spec.InterlaceField = 0;
   }
   else
      PrevInterlaced = false;

#endif

   if ((!PrevInterlaced && (width != rects[0] - h_mask)) ||
      (PrevInterlaced && (width != FB_WIDTH)) ||
      source_height != spec.DisplayRect.h)
         resolution_changed = true;

   source_height  =  spec.DisplayRect.h;
   linevisfirst   =  (is_pal ? first_sl_pal : first_sl) << PrevInterlaced;
   overscan_mask  =  PrevInterlaced ? 0 : h_mask >> 1;
   width          =  PrevInterlaced ? FB_WIDTH : (rects[0] - h_mask);
   height         =  (is_pal ? (last_sl_pal + 1 - first_sl_pal) :
                     (last_sl + 1 - first_sl)) << PrevInterlaced;

   video_cb(surf->pixels + surf->pitchinpix * linevisfirst + overscan_mask,
            width, height, FB_WIDTH << 2);

   if (resolution_changed)
      update_geometry(width, height);

   video_frames++;
   audio_frames += spec.SoundBufSize;

   int16_t *interbuf = (int16_t*)&IBuffer;

   audio_batch_cb(interbuf, spec.SoundBufSize);
}

void retro_get_system_info(struct retro_system_info *info)
{
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
   memset(info, 0, sizeof(*info));
   info->library_name     = MEDNAFEN_CORE_NAME;
   info->library_version  = MEDNAFEN_CORE_VERSION GIT_VERSION;
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

void retro_set_environment( retro_environment_t cb )
{
   environ_cb = cb;

   static const struct retro_variable vars[] = {
      { "beetle_saturn_region", "System Region; Auto Detect|Japan|North America|Europe|South Korea|Asia (NTSC)|Asia (PAL)|Brazil|Latin America" },
      { "beetle_saturn_cdimagecache", "CD Image Cache (restart); disabled|enabled" },
      { "beetle_saturn_autortc", "Automatically set RTC on game load; enabled|disabled" },
      { "beetle_saturn_autortc_lang", "BIOS language; english|german|french|spanish|italian|japanese" },
      { "beetle_saturn_horizontal_overscan", "Horizontal Overscan Mask; 0|2|4|6|8|10|12|14|16|18|20|22|24|26|28|30|32|34|36|38|40|42|44|46|48|50|52|54|56|58|60" },
      { "beetle_saturn_initial_scanline", "Initial scanline; 0|1|2|3|4|5|6|7|8|9|10|11|12|13|14|15|16|17|18|19|20|21|22|23|24|25|26|27|28|29|30|31|32|33|34|35|36|37|38|39|40" },
      { "beetle_saturn_last_scanline", "Last scanline; 239|210|211|212|213|214|215|216|217|218|219|220|221|222|223|224|225|226|227|228|229|230|231|232|233|234|235|236|237|238" },
      { "beetle_saturn_initial_scanline_pal", "Initial scanline PAL; 16|17|18|19|20|21|22|23|24|25|26|27|28|29|30|31|32|33|34|35|36|37|38|39|40|41|42|43|44|45|46|47|48|49|50|51|52|53|54|55|56|57|58|59|60|0|1|2|3|4|5|6|7|8|9|10|11|12|13|14|15" },
      { "beetle_saturn_last_scanline_pal", "Last scanline PAL; 271|272|273|274|275|276|277|278|279|280|281|282|283|284|285|286|287|230|231|232|233|234|235|236|237|238|239|240|241|242|243|244|245|246|247|248|249|250|251|252|253|254|255|256|257|258|259|260|261|262|263|264|265|266|267|268|269|270" },
      { "beetle_saturn_analog_stick_deadzone", "Analog Deadzone (percent); 15|20|25|30|0|5|10"},
      { NULL, NULL },
   };

   cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);

   input_set_env( cb );
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

static size_t serialize_size = 0;

size_t retro_serialize_size(void)
{
	// Don't know yet?
	if ( serialize_size == 0 )
	{
		// Do a fake save to see.
		StateMem st;
		memset( &st, 0, sizeof(st) );
		if ( MDFNSS_SaveSM( &st, 0, 0, NULL, NULL, NULL ) )
		{
			// Cache and tidy up.
			serialize_size = st.len;
			free( st.data );
		}
	}

	// Return cached value.
	return serialize_size;
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
               (!shared_memorycards) ? retro_cd_base_name : "mednafen_saturn_libretro_shared",
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
