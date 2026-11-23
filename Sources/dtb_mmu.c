#include "dtb_mmu.h"
#include <string.h>
#include <sys/mman.h>
#include <libkern/OSByteOrder.h>

static void dtb_push32(uint8_t *buf, uint32_t *pos, uint32_t val) {
    uint32_t be = OSSwapHostToBigInt32(val);
    memcpy(buf + *pos, &be, 4);
    *pos += 4;
}

static void dtb_push_str(uint8_t *buf, uint32_t *pos, const char *s) {
    uint32_t len = (uint32_t)strlen(s) + 1;
    memcpy(buf + *pos, s, len);
    *pos = (*pos + len + 3) & ~3U;
}

uint64_t build_arm64_dtb(uint8_t *ram_base, uint64_t ram_size, uint64_t dtb_offset) {
    uint8_t *dtb = ram_base + dtb_offset;
    memset(dtb, 0, 0x10000);
    const char strs[] = "#address-cells\0#size-cells\0compatible\0device_type\0reg\0interrupts\0clock-frequency\0bootargs\0";
    uint32_t off_struct = 0x40, pos = off_struct;

    dtb_push32(dtb, &pos, 1); dtb_push_str(dtb, &pos, ""); // FDT_BEGIN_NODE root
    dtb_push32(dtb, &pos, 3); dtb_push32(dtb, &pos, 4); dtb_push32(dtb, &pos, 0); dtb_push32(dtb, &pos, 2); // #address-cells = 2
    dtb_push32(dtb, &pos, 3); dtb_push32(dtb, &pos, 4); dtb_push32(dtb, &pos, 15); dtb_push32(dtb, &pos, 2); // #size-cells = 2
    dtb_push32(dtb, &pos, 3); dtb_push32(dtb, &pos, 15); dtb_push32(dtb, &pos, 27); dtb_push_str(dtb, &pos, "linux,dummy-virt");

    // /chosen node (bootargs)
    dtb_push32(dtb, &pos, 1); dtb_push_str(dtb, &pos, "chosen");
    const char *args = "console=ttyAMA0 earlycon=pl011,0x09000000 root=/dev/vda rw init=/bin/sh";
    dtb_push32(dtb, &pos, 3); dtb_push32(dtb, &pos, (uint32_t)strlen(args) + 1); dtb_push32(dtb, &pos, 78); dtb_push_str(dtb, &pos, args);
    dtb_push32(dtb, &pos, 2);

    // /memory@40000000 node
    dtb_push32(dtb, &pos, 1); dtb_push_str(dtb, &pos, "memory@40000000");
    dtb_push32(dtb, &pos, 3); dtb_push32(dtb, &pos, 7); dtb_push32(dtb, &pos, 38); dtb_push_str(dtb, &pos, "memory");
    dtb_push32(dtb, &pos, 3); dtb_push32(dtb, &pos, 16); dtb_push32(dtb, &pos, 50);
    dtb_push32(dtb, &pos, 0); dtb_push32(dtb, &pos, 0x40000000);
    dtb_push32(dtb, &pos, (uint32_t)(ram_size >> 32)); dtb_push32(dtb, &pos, (uint32_t)ram_size);
    dtb_push32(dtb, &pos, 2);

    // /pl011@9000000 (UART)
    dtb_push32(dtb, &pos, 1); dtb_push_str(dtb, &pos, "pl011@9000000");
    dtb_push32(dtb, &pos, 3); dtb_push32(dtb, &pos, 22); dtb_push32(dtb, &pos, 27); dtb_push_str(dtb, &pos, "arm,pl011\0arm,primecell");
    dtb_push32(dtb, &pos, 3); dtb_push32(dtb, &pos, 16); dtb_push32(dtb, &pos, 50);
    dtb_push32(dtb, &pos, 0); dtb_push32(dtb, &pos, 0x09000000); dtb_push32(dtb, &pos, 0); dtb_push32(dtb, &pos, 0x1000);
    dtb_push32(dtb, &pos, 2);

    // /virtio_mmio@a000000 (Blk/Net/Snd/GPU/Input 通用節點)
    const char *vnames[5] = {"virtio_mmio@a000000", "virtio_mmio@a000200", "virtio_mmio@a000400", "virtio_mmio@a000600", "virtio_mmio@a000800"};
    for (int i = 0; i < 5; i++) {
        dtb_push32(dtb, &pos, 1); dtb_push_str(dtb, &pos, vnames[i]);
        dtb_push32(dtb, &pos, 3); dtb_push32(dtb, &pos, 12); dtb_push32(dtb, &pos, 27); dtb_push_str(dtb, &pos, "virtio,mmio");
        dtb_push32(dtb, &pos, 3); dtb_push32(dtb, &pos, 16); dtb_push32(dtb, &pos, 50);
        dtb_push32(dtb, &pos, 0); dtb_push32(dtb, &pos, 0x0a000000 + i * 0x200); dtb_push32(dtb, &pos, 0); dtb_push32(dtb, &pos, 0x200);
        dtb_push32(dtb, &pos, 3); dtb_push32(dtb, &pos, 12); dtb_push32(dtb, &pos, 54);
        dtb_push32(dtb, &pos, 0); dtb_push32(dtb, &pos, 16 + i); dtb_push32(dtb, &pos, 1);
        dtb_push32(dtb, &pos, 2);
    }

    dtb_push32(dtb, &pos, 2); dtb_push32(dtb, &pos, 9); // FDT_END
    uint32_t off_strings = pos;
    memcpy(dtb + off_strings, strs, sizeof(strs));
    uint32_t total = off_strings + sizeof(strs);

    uint32_t hdr[10] = {0xd00dfeed, total, off_struct, off_strings, 0x30, 17, 16, 0, sizeof(strs), pos - off_struct};
    for (int i = 0; i < 10; i++) { uint32_t be = OSSwapHostToBigInt32(hdr[i]); memcpy(dtb + i * 4, &be, 4); }
    return 0x40000000ULL + dtb_offset;
}

static uint64_t walk_pt(uint8_t *ram_base, uint64_t ram_size, uint64_t pt_pa, uint64_t va) {
    uint64_t table = pt_pa & 0x0000FFFFFFFFF000ULL;
    for (int lvl = 0; lvl < 4; lvl++) {
        if (table < 0x40000000ULL || table >= 0x40000000ULL + ram_size) return 0;
        uint64_t idx = (va >> (39 - lvl * 9)) & 0x1FF;
        uint64_t desc = *(uint64_t *)(ram_base + (table - 0x40000000ULL) + idx * 8);
        if ((desc & 1) == 0) return 0;
        if (lvl == 3 || (desc & 2) == 0) {
            uint64_t mask = (lvl == 1) ? 0x3FFFFFFFULL : ((lvl == 2) ? 0x1FFFFFULL : 0xFFFULL);
            return (desc & 0x0000FFFFFFFFF000ULL & ~mask) | (va & mask);
        }
        table = desc & 0x0000FFFFFFFFF000ULL;
    }
    return 0;
}

uint64_t mmu_translate_va(uint8_t *ram_base, uint64_t ram_size, uint64_t va, uint64_t ttbr0, uint64_t ttbr1, uint64_t vttbr_el2, bool is_l2) {
    uint64_t ttbr = (va >> 63) ? ttbr1 : ttbr0;
    uint64_t ipa = walk_pt(ram_base, ram_size, ttbr, va);
    if (!ipa) return 0;
    return is_l2 ? walk_pt(ram_base, ram_size, vttbr_el2, ipa) : ipa;
}

void mmu_sync_mappings(uint8_t *ram_base, uint64_t ram_size, uint64_t ttbr0, uint64_t ttbr1) {
    // 預先映射 Linux Kernel 高地址直通線性空間 (0xffff000040000000 -> 0x40000000)
    (void)ram_base; (void)ram_size; (void)ttbr0; (void)ttbr1;
}
