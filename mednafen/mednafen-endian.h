#ifndef __MDFN_ENDIAN_H
#define __MDFN_ENDIAN_H

#include <stdio.h>
#include <stdint.h>
#include <retro_inline.h>
#include "mednafen-types.h"

#if 0
#if MDFN_GCC_VERSION >= MDFN_MAKE_GCCV(4,7,0)
#define MDFN_ASSUME_ALIGNED(p, align) ((decltype(p))__builtin_assume_aligned((p), (align)))
#else
#define MDFN_ASSUME_ALIGNED(p, align) (p)
#endif
#else
#define MDFN_ASSUME_ALIGNED(p, align) ((decltype(p))__builtin_assume_aligned((p), (align)))
#endif

#ifdef MSB_FIRST
 #define MDFN_ENDIANH_IS_BIGENDIAN 1
#else
 #define MDFN_ENDIANH_IS_BIGENDIAN 0
#endif

#ifdef MSB_FIRST
#ifndef le32toh
#define le32toh(l)      ((((l)>>24) & 0xff) | (((l)>>8) & 0xff00) \
      | (((l)<<8) & 0xff0000) | (((l)<<24) & 0xff000000))
#endif
#ifndef le16toh
#define le16toh(l)      ((((l)>>8) & 0xff) | (((l)<<8) & 0xff00))
#endif
#else
#ifndef le32toh
#define le32toh(l)      (l)
#endif
#ifndef le16toh
#define le16toh(l)      (l)
#endif
#endif

#ifndef htole32
#define htole32 le32toh
#endif

#ifndef htole16
#define htole16 le16toh
#endif


#if defined(__cplusplus)
extern "C" {
#endif

int read32le(uint32_t *Bufo, FILE *fp);
void Endian_A16_Swap(void *src, uint32_t nelements);
void Endian_A32_Swap(void *src, uint32_t nelements);
void Endian_A64_Swap(void *src, uint32_t nelements);

void Endian_A16_LE_to_NE(void *src, uint32_t nelements);
void Endian_A16_BE_to_NE(void *src, uint32_t nelements);
void Endian_A32_LE_to_NE(void *src, uint32_t nelements);
void Endian_A64_LE_to_NE(void *src, uint32_t nelements);

void FlipByteOrder(uint8_t *src, uint32_t count);

#if defined(__cplusplus)
}
#endif

// The following functions can encode/decode to unaligned addresses.

static inline void MDFN_en16lsb(uint8_t *buf, uint16_t morp)
{
 buf[0]=morp;
 buf[1]=morp>>8;
}

static inline void MDFN_en24lsb(uint8_t *buf, uint32_t morp)
{
 buf[0]=morp;
 buf[1]=morp>>8;
 buf[2]=morp>>16;
}


static inline void MDFN_en32lsb(uint8_t *buf, uint32_t morp)
{
 buf[0]=morp;
 buf[1]=morp>>8;
 buf[2]=morp>>16;
 buf[3]=morp>>24;
}

static inline void MDFN_en64lsb(uint8_t *buf, uint64_t morp)
{
 buf[0]=morp >> 0;
 buf[1]=morp >> 8;
 buf[2]=morp >> 16;
 buf[3]=morp >> 24;
 buf[4]=morp >> 32;
 buf[5]=morp >> 40;
 buf[6]=morp >> 48;
 buf[7]=morp >> 56;
}


static inline void MDFN_en16msb(uint8_t *buf, uint16_t morp)
{
 buf[0] = morp >> 8;
 buf[1] = morp;
}

static inline void MDFN_en24msb(uint8_t *buf, uint32_t morp)
{
 buf[0] = morp >> 16;
 buf[1] = morp >> 8;
 buf[2] = morp;
}

static inline void MDFN_en32msb(uint8_t *buf, uint32_t morp)
{
 buf[0] = morp >> 24;
 buf[1] = morp >> 16;
 buf[2] = morp >> 8;
 buf[3] = morp;
}

static inline void MDFN_en64msb(uint8_t *buf, uint64_t morp)
{
 buf[0] = morp >> 56;
 buf[1] = morp >> 48;
 buf[2] = morp >> 40;
 buf[3] = morp >> 32;
 buf[4] = morp >> 24;
 buf[5] = morp >> 16;
 buf[6] = morp >> 8;
 buf[7] = morp >> 0;
}

static inline uint16_t MDFN_de16lsb(const uint8_t *morp)
{
 return(morp[0] | (morp[1] << 8));
}

static inline uint32_t MDFN_de24lsb(const uint8_t *morp)
{
 return(morp[0]|(morp[1]<<8)|(morp[2]<<16));
}

static inline uint32_t MDFN_de32lsb(const uint8_t *morp)
{
 return(morp[0]|(morp[1]<<8)|(morp[2]<<16)|(morp[3]<<24));
}

static inline uint64_t MDFN_de64lsb(const uint8_t *morp)
{
 uint64_t ret = 0;

 ret |= (uint64_t)morp[0];
 ret |= (uint64_t)morp[1] << 8;
 ret |= (uint64_t)morp[2] << 16;
 ret |= (uint64_t)morp[3] << 24;
 ret |= (uint64_t)morp[4] << 32;
 ret |= (uint64_t)morp[5] << 40;
 ret |= (uint64_t)morp[6] << 48;
 ret |= (uint64_t)morp[7] << 56;

 return(ret);
}

/*
 Regarding safety of calling MDFN_*sb<true> on dynamically-allocated memory with new uint8[], see C++ standard 3.7.3.1(i.e. it should be
 safe provided the offsets into the memory are aligned/multiples of the MDFN_*sb access type).  malloc()'d and calloc()'d
 memory should be safe as well.

 Statically-allocated arrays/memory should be unioned with a big POD type or C++11 "alignas"'d.  (May need to audit code to ensure
 this is being done).
*/

static INLINE uint16 MDFN_bswap16(uint16 v)
{
 return (v << 8) | (v >> 8);
}

static INLINE uint32 MDFN_bswap32(uint32 v)
{
 return (v << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
}

static INLINE uint64 MDFN_bswap64(uint64 v)
{
 return (v << 56) | (v >> 56) | ((v & 0xFF00) << 40) | ((v >> 40) & 0xFF00) | ((uint64)MDFN_bswap32(v >> 16) << 16);
}


#ifdef __cplusplus

//
// X endian.
//
template<int isbigendian, typename T, bool aligned>
static INLINE T MDFN_deXsb(const void* ptr)
{
 T tmp;

 memcpy(&tmp, MDFN_ASSUME_ALIGNED(ptr, (aligned ? sizeof(T) : 1)), sizeof(T));

 if(isbigendian != -1 && isbigendian != MDFN_ENDIANH_IS_BIGENDIAN)
 {
  static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8, "Gummy penguins.");

  if(sizeof(T) == 8)
   return MDFN_bswap64(tmp);
  else if(sizeof(T) == 4)
   return MDFN_bswap32(tmp);
  else if(sizeof(T) == 2)
   return MDFN_bswap16(tmp);
 }

 return tmp;
}

template<typename T, bool aligned = false>
static INLINE T MDFN_demsb(const void* ptr)
{
 return MDFN_deXsb<1, T, aligned>(ptr);
}

template<bool aligned = false>
static INLINE uint16 MDFN_de16msb(const void* ptr)
{
 return MDFN_demsb<uint16, aligned>(ptr);
}

#endif

static inline uint32_t MDFN_de24msb(const uint8_t *morp)
{
 return((morp[2]<<0)|(morp[1]<<8)|(morp[0]<<16));
}


static inline uint32_t MDFN_de32msb(const uint8_t *morp)
{
 return(morp[3]|(morp[2]<<8)|(morp[1]<<16)|(morp[0]<<24));
}

#endif
