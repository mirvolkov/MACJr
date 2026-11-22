#ifndef _STDARG_H
#define _STDARG_H
/* Variable arguments, as GCC lays them out on Xtensa (va_list is in
 * tccdefs.h). va_arg(): align the index for 8-byte types, step past the
 * argument; an argument is in the registers if it ends within the first 24
 * bytes, otherwise on the stack, never split (the first one there moves the
 * index to 32). */
typedef __builtin_va_list va_list;
typedef __builtin_va_list __gnuc_va_list;

#define __va_size(type) ((sizeof(type) + 3) & ~3)

#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap) ((void)0)
#define va_copy(dst, src) ((dst) = (src))
#define __va_copy(dst, src) ((dst) = (src))

#define va_arg(ap, type) (*(type *)(                                       \
    (ap).__va_ndx = (__alignof__(type) > 4 ? ((ap).__va_ndx + 7) & ~7      \
                                            : (ap).__va_ndx)               \
                    + __va_size(type),                                     \
    (ap).__va_ndx <= 24                                                    \
        ? (char *)(ap).__va_reg + (ap).__va_ndx - __va_size(type)          \
        : ((ap).__va_ndx - __va_size(type) <= 24                           \
               ? (ap).__va_ndx = 32 + __va_size(type) : 0,                 \
           (char *)(ap).__va_stk + (ap).__va_ndx - __va_size(type))))

#endif
