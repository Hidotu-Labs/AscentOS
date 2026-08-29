#ifndef CPU_FEATURES_H
#define CPU_FEATURES_H

#include <stdbool.h>
#include <stdint.h>

void cpu_features_init(void);
bool cpu_has_pcid(void);
bool cpu_has_invpcid(void);
bool cpu_has_xsave(void);
bool cpu_has_avx(void);
bool cpu_has_fsgsbase(void);
extern bool cpu_has_xsave_flag;

static inline void wrfsbase(uint64_t val) {
  __asm__ volatile("wrfsbase %0" : : "r"(val) : "memory");
}

static inline uint64_t rdfsbase(void) {
  uint64_t val;
  __asm__ volatile("rdfsbase %0" : "=r"(val));
  return val;
}

#endif

