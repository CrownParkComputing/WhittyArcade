/* Minimal mamesf.h shim for standalone Musashi softfloat build.
   Provides the integer typedefs and macros softfloat-macros / softfloat.c
   reference.  Avoids redefining uint64 (m68kcpu.h owns that). */
#ifndef MAMESF_H
#define MAMESF_H
#include <stdint.h>
#ifndef FLAG_TYPE_DEFINED
#define FLAG_TYPE_DEFINED
typedef int flag;
#endif
typedef uint8_t  bits8;   typedef int8_t  sbits8;
typedef uint16_t bits16;  typedef int16_t sbits16;
typedef uint32_t bits32;  typedef int32_t sbits32;
/* Musashi names its 64-bit working type unsigned long long.  Match that exact
   C type (not merely its width) because SoftFloat passes pointers between the
   two typedef families and GCC 16 diagnoses LP64's unsigned long mismatch. */
typedef unsigned long long bits64;
typedef signed long long sbits64;
typedef int8_t   int8;    typedef uint8_t  uint8x;
typedef int16_t  int16;
typedef int32_t  int32;
typedef int64_t  int64;
#ifndef LIT64
#define LIT64(x) x##ULL
#endif
#endif
