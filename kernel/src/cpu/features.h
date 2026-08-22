#ifndef CPU_FEATURES_H
#define CPU_FEATURES_H

#include <stdbool.h>

void cpu_features_init(void);
bool cpu_has_pcid(void);
bool cpu_has_invpcid(void);

#endif

