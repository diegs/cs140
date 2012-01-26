

#ifndef __FIXED_POINT_H__
#define __FIXED_POINT_H__

#include <stdint.h>

/* This represents 14 bits of sub-integer precision */
#define FP_PRECISION 16384


typedef int32_t fp_t;

static inline fp_t int2fp(int32_t num) {
  return num*FP_PRECISION;
}

static inline int32_t fp2int(fp_t fp) {
  return fp/FP_PRECISION;
}

static inline fp_t fpadd(fp_t first, fp_t second) {
  return first + second;
}

static inline fp_t fpsub(fp_t first, fp_t second) {
  return first - second;
}

static inline fp_t fpmul(fp_t first, fp_t second) {
  int64_t product = ((int64_t)first)*second/FP_PRECISION;
  return product;
}

static inline fp_t fpdiv(fp_t first, fp_t second) {
  int64_t quotient = ((int64_t)first)*FP_PRECISION/second;
  return quotient;
}


#endif

