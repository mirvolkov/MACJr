/* tccdefs.h -- read by the compiler before every source (MACJr, Xtensa).
 * The machine and the OS: 32-bit little-endian Xtensa, no hosted OS. */
#define __SIZE_TYPE__ unsigned int
#define __PTRDIFF_TYPE__ int
#define __ILP32__ 1
#define __INT64_TYPE__ long long
#define __INT32_TYPE__ int
#define __SIZEOF_INT__ 4
#define __INT_MAX__ 0x7fffffff
#define __LONG_MAX__ 0x7fffffffL
#define __SIZEOF_LONG_LONG__ 8
#define __LONG_LONG_MAX__ 0x7fffffffffffffffLL
#define __CHAR_BIT__ 8
#define __ORDER_LITTLE_ENDIAN__ 1234
#define __ORDER_BIG_ENDIAN__ 4321
#define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__
#define __WCHAR_TYPE__ int
#define __WINT_TYPE__ int
#define __UINTPTR_TYPE__ unsigned int
#define __INTPTR_TYPE__ int
#define __PRETTY_FUNCTION__ __FUNCTION__
#define __has_builtin(x) 0
#define __has_feature(x) 0
#define __has_attribute(x) 0
#ifndef __TCC_PP__
#define __builtin_offsetof(type, field) ((__SIZE_TYPE__)&((type *)0)->field)
#define __builtin_extract_return_addr(x) x
#endif
