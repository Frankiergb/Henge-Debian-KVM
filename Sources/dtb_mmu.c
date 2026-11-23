#include "dtb_mmu.h"
#include <string.h>
#include <sys/mman.h>
#include <libkern/OSByteOrder.h>

static void dtb_w32(uint8_t *b, uint32_t *p, uint32_t v) { uint32_t be = OSSwapHostToBigInt32(v); memcpy(b + *p, &be, 4); *p += 4; }
static void dtb_str(uint8_t *b, uint32_t *p, const char *s) { uint32_t l = (uint32_t)strlen(s) + 1; memcpy(b + *p, s, l); *p = (*p + l + 3) & ~3U; }

uint64_t build_arm64_dtb(uint8_t *ram_base, uint64_t ram_size, uint64_t dtb_offset) {
    uint8_t *dtb = ram_base + dtb_offset; memset(dtb, 0, 0x10000);
    const char *strs = "#address-cells\0#size-cells\0compatible\0device_type\0reg\0bootargs\0";
    uint32_t pos = 0x40, zero = 0;
    dtb_w32(dtb, &pos, 1); dtb_str(dtb, &pos, "");
    dtb_w32(dtb, &pos, 3); dtb_w32(dtb, &pos, 4); dtb_w32(dtb, &pos, 0); dtb_w32(dtb, &pos, 2);
    dtb_w32(dtb, &pos, 3); dtb_w32(dtb, &pos, 4); dtb_w32(dtb, &pos, 15); dtb_w32(dtb, &pos, 2);
    dtb_w32(dtb, &pos, 3); dtb_w32(dtb, &pos, 15); dtb_w32(dtb, &pos, 27); dtb_str(dtb, &pos, "linux,dummy-virt");
    dtb_w32(dtb, &pos, 1); dtb_str(dtb, &pos, "chosen");
    const char *args = "console=ttyAMA0 earlycon=pl011,0x09000000 root=/dev/vda1 rw";
    dtb_w32(dtb, &pos, 3); dtb_w32(dtb, &pos, (uint32_t)strlen(args) + 1); dtb_w32(dtb, &pos, 54); dtb_str(dtb, &pos, args);
    dtb_w32(dtb, &pos, 2);
    dtb_w32(dtb, &pos, 1); dtb_str(dtb, &pos, "memory@40000000");
    dtb_w32(dtb, &pos, 3); dtb_w32(dtb, &pos, 7); dtb_w32(dtb, &pos, 38); dtb_str(dtb, &pos, "memory");
    dtb_w32(dtb, &pos, 3); dtb_w32(dtb, &pos, 16); dtb_w32(dtb, &pos, 50);
    dtb_w32(dtb, &pos, 0); dtb_w32(dtb, &pos, 0x40000000); dtb_w32(dtb, &pos, 0); dtb_w32(dtb, &pos, (uint32_t)ram_size);
    dtb_w32(dtb, &pos, 2); dtb_w32(dtb, &pos, 2); dtb_w32(dtb, &pos, 9);
    memcpy(dtb + pos, strs, 63);
    dtb_w32(dtb, &zero, 0xd00dfeed); dtb_w32(dtb, &zero, pos + 63); dtb_w32(dtb, &zero, 0x40); dtb_w32(dtb, &zero, pos);
    dtb_w32(dtb, &zero, 0x30); dtb_w32(dtb, &zero, 17); dtb_w32(dtb, &zero, 16); dtb_w32(dtb, &zero, 0);
    dtb_w32(dtb, &zero, 63); dtb_w32(dtb, &zero, pos - 0x40);
    return 0x40000000ULL + dtb_offset;
}

uint64_t mmu_translate_va(uint8_t *ram, uint64_t rsz, uint64_t va, uint64_t t0, uint64_t t1, uint64_t vt, bool l2) {
    (void)ram; (void)rsz; (void)t0; (void)t1; (void)vt; (void)l2;
    return va & 0x0000FFFFFFFFFFFFULL;
}

void mmu_sync_mappings(uint8_t *ram, uint64_t rsz, uint64_t t0, uint64_t t1) {
    (void)t0; (void)t1;
    void *hi = mmap((void *)0xffff000040000000ULL, rsz, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (hi != MAP_FAILED) memcpy(hi, ram, rsz);
}
