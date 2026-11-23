#include "dtb_mmu.h"
#include <string.h>
#include <sys/mman.h>
#include <libkern/OSByteOrder.h>

static void dtb_push32(uint8_t *b, uint32_t *p, uint32_t v) { uint32_t be = OSSwapHostToBigInt32(v); memcpy(b + *p, &be, 4); *p += 4; }
static void dtb_push_str(uint8_t *b, uint32_t *p, const char *s) { uint32_t l = (uint32_t)strlen(s) + 1; memcpy(b + *p, s, l); *p = (*p + l + 3) & ~3U; }

uint64_t build_arm64_dtb(uint8_t *ram_base, uint64_t ram_size, uint64_t dtb_offset) {
    uint8_t *dtb = ram_base + dtb_offset; memset(dtb, 0, 0x10000);
    const char strs[] = "#address-cells\0#size-cells\0compatible\0device_type\0reg\0interrupts\0bootargs\0";
    uint32_t pos = 0x40;
    dtb_push32(dtb, &pos, 1); dtb_push_str(dtb, &pos, "");
    dtb_push32(dtb, &pos, 3); dtb_push32(dtb, &pos, 4); dtb_push32(dtb, &pos, 0); dtb_push32(dtb, &pos, 2);
    dtb_push32(dtb, &pos, 3); dtb_push32(dtb, &pos, 4); dtb_push32(dtb, &pos, 15); dtb_push32(dtb, &pos, 2);
    dtb_push32(dtb, &pos, 3); dtb_push32(dtb, &pos, 15); dtb_push32(dtb, &pos, 27); dtb_push_str(dtb, &pos, "linux,dummy-virt");
    dtb_push32(dtb, &pos, 1); dtb_push_str(dtb, &pos, "chosen");
    const char *args = "console=ttyAMA0 earlycon=pl011,0x09000000 root=/dev/vda1 rw";
    dtb_push32(dtb, &pos, 3); dtb_push32(dtb, &pos, (uint32_t)strlen(args) + 1); dtb_push32(dtb, &pos, 62); dtb_push_str(dtb, &pos, args);
    dtb_push32(dtb, &pos, 2);
    dtb_push32(dtb, &pos, 1); dtb_push_str(dtb, &pos, "memory@40000000");
    dtb_push32(dtb, &pos, 3); dtb_push32(dtb, &pos, 7); dtb_push32(dtb, &pos, 38); dtb_push_str(dtb, &pos, "memory");
    dtb_push32(dtb, &pos, 3); dtb_push32(dtb, &pos, 16); dtb_push32(dtb, &pos, 50);
    dtb_push32(dtb, &pos, 0); dtb_push32(dtb, &pos, 0x40000000); dtb_push32(dtb, &pos, 0); dtb_push32(dtb, &pos, (uint32_t)ram_size);
    dtb_push32(dtb, &pos, 2);
    dtb_push32(dtb, &pos, 2); dtb_push32(dtb, &pos, 9);
    memcpy(dtb + pos, strs, sizeof(strs));
    uint32_t hdr = {0xd00dfeed, pos + (uint32_t)sizeof(strs), 0x40, pos, 0x30, 17, 16, 0, sizeof(strs), pos - 0x40};
    for (int i = 0; i < 10; i++) { uint32_t be = OSSwapHostToBigInt32(hdr[i]); memcpy(dtb + i * 4, &be, 4); }
    return 0x40000000ULL + dtb_offset;
}

static uint64_t walk_pt(uint8_t *ram, uint64_t rsz, uint64_t pt, uint64_t va) {
    uint64_t tbl = pt & 0x0000FFFFFFFFF000ULL;
    for (int l = 0; l < 4; l++) {
        if (tbl < 0x40000000ULL || tbl >= 0x40000000ULL + rsz) return 0;
        uint64_t desc = *(uint64_t *)(ram + (tbl - 0x40000000ULL) + ((va >> (39 - l * 9)) & 0x1FF) * 8);
        if ((desc & 1) == 0) return 0;
        if (l == 3 || (desc & 2) == 0) {
            uint64_t m = (l == 1) ? 0x3FFFFFFFULL : ((l == 2) ? 0x1FFFFFULL : 0xFFFULL);
            return (desc & 0x0000FFFFFFFFF000ULL & ~m) | (va & m);
        }
        tbl = desc & 0x0000FFFFFFFFF000ULL;
    }
    return 0;
}

uint64_t mmu_translate_va(uint8_t *ram, uint64_t rsz, uint64_t va, uint64_t t0, uint64_t t1, uint64_t vt, bool l2) {
    uint64_t ipa = walk_pt(ram, rsz, (va >> 63) ? t1 : t0, va);
    return (!ipa) ? 0 : (l2 ? walk_pt(ram, rsz, vt, ipa) : ipa);
}

void mmu_sync_mappings(uint8_t *ram, uint64_t rsz, uint64_t t0, uint64_t t1) {
    (void)t0; (void)t1;
    // 將 Linux Kernel 標準線性映射高地址窗口直接鏡像到 Guest RAM，實現零開銷虛擬記憶體直跑
    mmap((void *)0xffff000040000000ULL, rsz, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    memcpy((void *)0xffff000040000000ULL, ram, rsz);
}
