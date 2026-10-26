/* Only what the vendored core needs; resolved from the TV's libc.so.6. */
#ifndef TIZGB_STDLIB_H
#define TIZGB_STDLIB_H
#include <stddef.h>
void abort(void) __attribute__((noreturn));
void *malloc(size_t);
void *calloc(size_t, size_t);
void  free(void *);
#endif
