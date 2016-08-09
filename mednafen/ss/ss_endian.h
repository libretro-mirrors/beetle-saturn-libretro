#define MDFN_ASSUME_ALIGNED(p, align) (p)

#ifdef MSB_FIRST
 #define MDFN_ENDIANH_IS_BIGENDIAN 1
#else
 #define MDFN_ENDIANH_IS_BIGENDIAN 0
#endif

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

// X endian.
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

//
// Native endian.
//
template<typename T, bool aligned = false>
static INLINE T MDFN_densb(const void* ptr)
{
 return MDFN_deXsb<-1, T, aligned>(ptr);
}

template<typename T>
static INLINE uint8* ne64_ptr_be(uint64* const base, const size_t byte_offset)
{
#ifdef MSB_FIRST
  return (uint8*)base + (byte_offset &~ (sizeof(T) - 1));
#else
  return (uint8*)base + (((byte_offset &~ (sizeof(T) - 1)) ^ (8 - sizeof(T))));
#endif
}

template<typename T>
static INLINE void ne64_wbo_be(uint64* const base, const size_t byte_offset, const T value)
{
  uint8* const ptr = ne64_ptr_be<T>(base, byte_offset);

  static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8, "Unsupported type size");

  memcpy(MDFN_ASSUME_ALIGNED(ptr, sizeof(T)), &value, sizeof(T));
}

template<typename T, typename BT>
static INLINE uint8* ne16_ptr_be(BT* const base, const size_t byte_offset)
{
#ifdef MSB_FIRST
  return (uint8*)base + (byte_offset &~ (sizeof(T) - 1));
#else
  return (uint8*)base + (((byte_offset &~ (sizeof(T) - 1)) ^ (2 - std::min<size_t>(2, sizeof(T)))));
#endif
}

template<typename T>
static INLINE T ne16_rbo_be(const uint16* const base, const size_t byte_offset)
{
  uint8* const ptr = ne16_ptr_be<T>(base, byte_offset);

  static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4, "Unsupported type size");

  if(sizeof(T) == 4)
  {
   uint16* const ptr16 = (uint16*)ptr;
   T tmp;

   tmp  = ptr16[0] << 16;
   tmp |= ptr16[1];

   return tmp;
  }
  else
   return *(T*)ptr;
}

template<typename T>
static INLINE void ne16_wbo_be(uint16* const base, const size_t byte_offset, const T value)
{
  uint8* const ptr = ne16_ptr_be<T>(base, byte_offset);

  static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4, "Unsupported type size");

  if(sizeof(T) == 4)
  {
   uint16* const ptr16 = (uint16*)ptr;

   ptr16[0] = value >> 16;
   ptr16[1] = value;
  }
  else
   *(T*)ptr = value;
}

template<typename T>
static INLINE T ne64_rbo_be(uint64* const base, const size_t byte_offset)
{
  uint8* const ptr = ne64_ptr_be<T>(base, byte_offset);
  T ret;

  static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4, "Unsupported type size");

  memcpy(&ret, MDFN_ASSUME_ALIGNED(ptr, sizeof(T)), sizeof(T));

  return ret;
}

template<typename T, bool IsWrite>
static INLINE void ne64_rwbo_be(uint64* const base, const size_t byte_offset, T* value)
{
  if(IsWrite)
   ne64_wbo_be<T>(base, byte_offset, *value);
  else
   *value = ne64_rbo_be<T>(base, byte_offset);
}

template<typename T, bool IsWrite>
static INLINE void ne16_rwbo_be(uint16* const base, const size_t byte_offset, T* value)
{
  if(IsWrite)
   ne16_wbo_be<T>(base, byte_offset, *value);
  else
   *value = ne16_rbo_be<T>(base, byte_offset);
}

//
// X endian.
//
template<int isbigendian, typename T, bool aligned>
static INLINE void MDFN_enXsb(void* ptr, T value)
{
 T tmp = value;

 if(isbigendian != -1 && isbigendian != MDFN_ENDIANH_IS_BIGENDIAN)
 {
  static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8, "Gummy penguins.");

  if(sizeof(T) == 8)
   tmp = MDFN_bswap64(value);
  else if(sizeof(T) == 4)
   tmp = MDFN_bswap32(value);
  else if(sizeof(T) == 2)
   tmp = MDFN_bswap16(value);
 }

 memcpy(MDFN_ASSUME_ALIGNED(ptr, (aligned ? sizeof(T) : 1)), &tmp, sizeof(T));
}

//
// Native endian.
//
template<typename T, bool aligned = false>
static INLINE void MDFN_ennsb(void* ptr, T value)
{
 MDFN_enXsb<-1, T, aligned>(ptr, value);
}

//
// Little endian.
//
template<typename T, bool aligned = false>
static INLINE void MDFN_enlsb(void* ptr, T value)
{
 MDFN_enXsb<0, T, aligned>(ptr, value);
}
