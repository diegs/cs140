

#ifndef __FIXED_POINT_H__
#define __FIXED_POINT_H__

#include <math.h>
#include <stdint.h>

static const int32_t fp_f = 16384;

static inline int32_t int2fp(int32_t num) {
  return num*fp_f;
}

static inline int32_t fp2int(int32_t fp) {
  return num/fp_f;
}

static inline int32_t fpadd(int32_t first, int32_t second) {
  return first + second;
}

static inline int32_t fpsub(int32_t first, int32_t second) {
  return first - second;
}

static inline int32_t fpmul(int32_t first, int32_t second) {
  int64_t product = ((int64_t)first)*second/fp_f;
  return product;
}

static inline int32_t fpdiv(int32_t first, int32_t second) {
  int64_t quotient = ((int64_t)first)*fp_f/second;
  return quotient;
}

#endif

