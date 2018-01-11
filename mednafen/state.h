#ifndef _STATE_H
#define _STATE_H

#include <retro_inline.h>
#include <type_traits>

typedef struct
{
   uint8_t *data;
   uint32_t loc;
   uint32_t len;
   uint32_t malloced;
   uint32_t initial_malloc; // A setting!
} StateMem;

// Eh, we abuse the smem_* in-memory stream code
// in a few other places. :)
int32_t smem_read(StateMem *st, void *buffer, uint32_t len);
int32_t smem_write(StateMem *st, void *buffer, uint32_t len);
int32_t smem_putc(StateMem *st, int value);
int32_t smem_seek(StateMem *st, uint32_t offset, int whence);
int smem_write32le(StateMem *st, uint32_t b);
int smem_read32le(StateMem *st, uint32_t *b);

int MDFNSS_SaveSM(void *st, int, int, const void*, const void*, const void*);
int MDFNSS_LoadSM(void *st, int, int);

// Flag for a single, >= 1 byte native-endian variable
#define MDFNSTATE_RLSB            0x80000000

// 32-bit native-endian elements
#define MDFNSTATE_RLSB32          0x40000000

// 16-bit native-endian elements
#define MDFNSTATE_RLSB16          0x20000000

// 64-bit native-endian elements
#define MDFNSTATE_RLSB64          0x10000000

#define MDFNSTATE_BOOL		  0x08000000

struct SFORMAT
{
	const char* name;	// Name;
	void* data;		// Pointer to the variable/array
	uint32 size;		// Length, in bytes, of the data to be saved EXCEPT:
				//  In the case of 'bool' it is the number of bool elements to save(bool is not always 1-byte).
				// If 0, the subchunk isn't saved.
	uint32 type;		// Type/element size; 0(bool), 1, 2, 4, 8
	uint32 repcount;
	uint32 repstride;
};

static INLINE bool* SF_FORCE_AB(bool* p) { return p; }

static INLINE int8* SF_FORCE_A8(int8* p) { return p; }
static INLINE uint8* SF_FORCE_A8(uint8* p) { return p; }

static INLINE int16* SF_FORCE_A16(int16* p) { return p; }
static INLINE uint16* SF_FORCE_A16(uint16* p) { return p; }

static INLINE int32* SF_FORCE_A32(int32* p) { return p; }
static INLINE uint32* SF_FORCE_A32(uint32* p) { return p; }

static INLINE int64* SF_FORCE_A64(int64* p) { return p; }
static INLINE uint64* SF_FORCE_A64(uint64* p) { return p; }

static INLINE float* SF_FORCE_AF(float* p) { return p; }
static INLINE double* SF_FORCE_AD(double* p) { return p; }

template<typename T>
static INLINE void SF_FORCE_ANY(T* p, typename std::enable_if<!std::is_enum<T>::value>::type* = nullptr)
{
 static_assert(	std::is_same<T, bool>::value ||
		std::is_same<T, int8>::value || std::is_same<T, uint8>::value ||
		std::is_same<T, int16>::value || std::is_same<T, uint16>::value ||
		std::is_same<T, int32>::value || std::is_same<T, uint32>::value || std::is_same<T, float>::value ||
		std::is_same<T, int64>::value || std::is_same<T, uint64>::value || std::is_same<T, double>::value, "Unsupported type");
}

template<typename T>
static INLINE void SF_FORCE_ANY(T* p, typename std::enable_if<std::is_enum<T>::value>::type* = nullptr)
{
 SF_FORCE_ANY((typename std::underlying_type<T>::type*)p);
}

template<typename T>
static INLINE SFORMAT SFBASE_(T* const v, const uint32 count, const uint32 totalcount, const size_t repstride, const char* const name)
{
 SF_FORCE_ANY(v);
 //
 //
 SFORMAT ret;

 ret.data = v;
 ret.name = name;
 ret.repcount = totalcount - 1;
 ret.repstride = repstride;
 if(std::is_same<T, bool>::value)
 {
  ret.size = count;
  ret.type = 0;
 }
 else
 {
  ret.size = sizeof(T) * count;
  ret.type = sizeof(T);
 }

 return ret;
}

template<typename T>
static INLINE SFORMAT SFBASE_(T* const v, const uint32 count, const char* const name)
{
 return SFBASE_(v, count, 1, 0, name);
}

#define SFVARN(x, ...)	SFBASE_(&(x), 1, __VA_ARGS__)

#define SFVAR1_(x)	   SFVARN((x), #x)
#define SFVAR3_(x, tc, rs) SFVARN((x), tc, rs, #x)
#define SFVAR_(a, b, c, d, ...)	d

#define EXPAND( x ) x
#define SFVAR(...) 	EXPAND(SFVAR_(__VA_ARGS__, SFVAR3_, SFVAR2_, SFVAR1_, SFVAR0_)(__VA_ARGS__))

#if SIZEOF_DOUBLE != 8
 #error "sizeof(double) != 8"
#endif

#define SFARRAYN(x, ...)	SFBASE_(SF_FORCE_A8(x), __VA_ARGS__)
#define SFARRAY(x, ...)		SFBASE_(SF_FORCE_A8(x), __VA_ARGS__, #x)

#define SFARRAYBN(x, ...)	SFBASE_(SF_FORCE_AB(x), __VA_ARGS__)
#define SFARRAYB(x, ...)	SFBASE_(SF_FORCE_AB(x), __VA_ARGS__, #x)

#define SFARRAY16N(x, ...)	SFBASE_(SF_FORCE_A16(x), __VA_ARGS__)
#define SFARRAY16(x, ...)	SFBASE_(SF_FORCE_A16(x), __VA_ARGS__, #x)

#define SFARRAY32N(x, ...)	SFBASE_(SF_FORCE_A32(x), __VA_ARGS__)
#define SFARRAY32(x, ...)	SFBASE_(SF_FORCE_A32(x), __VA_ARGS__, #x)

#define SFARRAY64N(x, ...)	SFBASE_(SF_FORCE_A64(x), __VA_ARGS__)
#define SFARRAY64(x, ...)	SFBASE_(SF_FORCE_A64(x), __VA_ARGS__, #x)

#define SFARRAYFN(x, ...)	SFBASE_(SF_FORCE_AF(x), __VA_ARGS__)
#define SFARRAYF(x, ...)	SFBASE_(SF_FORCE_AF(x), __VA_ARGS__, #x)

#define SFARRAYDN(x, ...)	SFBASE_(SF_FORCE_AD(x), __VA_ARGS__)
#define SFARRAYD(x, ...)	SFBASE_(SF_FORCE_AD(x), __VA_ARGS__, #x)

#define SFLINK(x) { nullptr, (x), ~0U, 0, 0, 0 }

#define SFEND { nullptr, nullptr, 0, 0, 0, 0 }

#include <vector>

// State-Section Descriptor
struct SSDescriptor
{
   SFORMAT *sf;
   const char *name;
   bool optional;
};

int MDFNSS_StateAction(void *st, int load, int data_only, SFORMAT *sf, const char *name, bool optional);

#endif
