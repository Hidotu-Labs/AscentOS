#ifndef MM_TLB_SHOOTDOWN_H
#define MM_TLB_SHOOTDOWN_H

#include <stdint.h>

#define IPI_VECTOR_TLB_SHOOTDOWN 50

#define TLB_SHOOTDOWN_ALL UINT64_MAX

void tlb_shootdown_init(void);

void tlb_shootdown_page(uint64_t addr);

void tlb_shootdown_all(void);

void tlb_shootdown_handle_ipi(void);

#endif // MM_TLB_SHOOTDOWN_H
