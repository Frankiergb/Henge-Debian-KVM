#include "virtio.h"
#include "gic_timer.h"
#include <string.h>

extern bool virtio_blk_open(const char *path);
extern uint64_t virtio_blk_sectors(void);
extern void virtio_snd_gpu_init(void);

static uint8_t *g_ram = NULL;
static uint32_t v_status[5], v_qsel[5], v_qnum[5];
static uint64_t v_desc[5], v_avail[5], v_used[5];

void virtio_init(uint8_t *ram_base, uint64_t ram_size) {
    (void)ram_size; g_ram = ram_base;
    memset(v_status, 0, sizeof(v_status));
    virtio_snd_gpu_init();
}

bool virtio_attach_disk(const char *img_path) { return virtio_blk_open(img_path); }

uint64_t virtio_mmio_read(uint64_t addr, int size) {
    (void)size;
    int id = (int)((addr - 0x0a000000ULL) / 0x200ULL);
    uint64_t off = addr & 0x1FFULL;
    const uint32_t dev_ids[5] = {2, 1, 25, 16, 18}; // Blk, Net, Snd, GPU, Input
    if (id < 0 || id >= 5) return 0;
    if (off == 0x000) return 0x74726976; // "virt" Magic
    if (off == 0x004) return 2;          // Modern VirtIO v2
    if (off == 0x008) return dev_ids[id];
    if (off == 0x034) return 128;        // QueueNumMax
    if (off == 0x070) return v_status[id];
    if (off == 0x100 && id == 0) return virtio_blk_sectors();
    return 0;
}

void virtio_mmio_write(uint64_t addr, uint64_t val, int size) {
    (void)size;
    int id = (int)((addr - 0x0a000000ULL) / 0x200ULL);
    uint64_t off = addr & 0x1FFULL;
    if (id < 0 || id >= 5) return;
    if (off == 0x030) v_qsel[id] = (uint32_t)val;
    else if (off == 0x038) v_qnum[id] = (uint32_t)val;
    else if (off == 0x050) gic_set_irq(48 + id, true); // QueueNotify 觸發 SPI 中斷
    else if (off == 0x070) v_status[id] = (uint32_t)val;
    else if (off == 0x080) v_desc[id] = (v_desc[id] & ~0xFFFFFFFFULL) | val;
    else if (off == 0x084) v_desc[id] = (v_desc[id] & 0xFFFFFFFFULL) | (val << 32);
    else if (off == 0x090) v_avail[id] = (v_avail[id] & ~0xFFFFFFFFULL) | val;
    else if (off == 0x094) v_avail[id] = (v_avail[id] & 0xFFFFFFFFULL) | (val << 32);
    else if (off == 0x0a0) v_used[id] = (v_used[id] & ~0xFFFFFFFFULL) | val;
    else if (off == 0x0a4) v_used[id] = (v_used[id] & 0xFFFFFFFFULL) | (val << 32);
}

void virtio_push_input(uint16_t type, uint16_t code, int32_t value) {
    (void)type; (void)code; (void)value;
    gic_set_irq(52, true); // 觸發 VirtIO-Input 中斷
}
