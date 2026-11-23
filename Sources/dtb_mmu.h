#ifndef DTB_MMU_H
#define DTB_MMU_H

#include <stdint.h>
#include <stdbool.h>

uint64_t build_arm64_dtb(uint8_t *ram_base, uint64_t ram_size, uint64_t dtb_offset);
uint64_t mmu_translate_va(uint8_t *ram_base, uint64_t ram_size, uint64_t va, uint64_t ttbr0, uint64_t ttbr1, uint64_t vttbr_el2, bool is_l2);
void mmu_sync_mappings(uint8_t *ram_base, uint64_t ram_size, uint64_t ttbr0, uint64_t ttbr1);

#endif
