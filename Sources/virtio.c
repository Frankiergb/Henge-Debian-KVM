#include "virtio.h"
#include "gic_timer.h"
#include <stdlib.h>
#include <string.h>

extern bool virtio_blk_open(const char *path);
extern uint64_t virtio_blk_sectors(void);
extern uint8_t virtio_blk_process(uint32_t type, uint64_t sector, uint8_t *buf, uint32_t len);
extern void virtio_snd_gpu_init(void);
extern void virtio_snd_play_pcm(const uint8_t *pcm, uint32_t len);

static uint8_t *g_ram = NULL;
static uint32_t *v_status = NULL, *v_qsel = NULL, *v_qnum = NULL, *v_last_avail = NULL;
static uint64_t *v_desc = NULL, *v_avail = NULL, *v_used = NULL;

void virtio_init(uint8_t *ram_base, uint64_t ram_size) {
    (void)ram_size; g_ram = ram_base;
    if (!v_status) {
        v_status = calloc(5, 4); v_qsel = calloc(5, 4); v_qnum = calloc(5, 4); v_last_avail = calloc(5, 4);
        v_desc = calloc(5, 8); v_avail = calloc(5, 8); v_used = calloc(5, 8);
    }
    virtio_snd_gpu_init();
}

bool virtio_attach_disk(const char *img_path) { return virtio_blk_open(img_path); }

static void kick_vq(int id) {
    if (!g_ram || !v_desc) return;
    uint64_t d_pa = *(v_desc + id), a_pa = *(v_avail + id), u_pa = *(v_used + id);
    uint32_t qsz = *(v_qnum + id);
    if (d_pa < 0x40000000ULL || a_pa < 0x40000000ULL || u_pa < 0x40000000ULL || qsz == 0) return;
    uint8_t *desc = g_ram + (d_pa - 0x40000000ULL);
    uint16_t *avail = (uint16_t *)(g_ram + (a_pa - 0x40000000ULL));
    uint16_t *used = (uint16_t *)(g_ram + (u_pa - 0x40000000ULL));
    uint16_t a_idx = *(avail + 1), l_idx = *(v_last_avail + id);

    while (l_idx != a_idx) {
        uint16_t head = *(avail + 2 + (l_idx % qsz));
        uint64_t addr0 = *(uint64_t *)(desc + head * 16);
        uint32_t len0 = *(uint32_t *)(desc + head * 16 + 8);
        uint16_t flags0 = *(uint16_t *)(desc + head * 16 + 12);
        uint16_t next0 = *(uint16_t *)(desc + head * 16 + 14);
        if (id == 0 && (flags0 & 1) && addr0 >= 0x40000000ULL) {
            uint32_t *hdr = (uint32_t *)(g_ram + (addr0 - 0x40000000ULL));
            uint64_t sec = *(uint64_t *)(hdr + 2);
            uint64_t addr1 = *(uint64_t *)(desc + next0 * 16);
            uint32_t len1 = *(uint32_t *)(desc + next0 * 16 + 8);
            uint16_t next1 = *(uint16_t *)(desc + next0 * 16 + 14);
            uint64_t addr2 = *(uint64_t *)(desc + next1 * 16);
            if (addr1 >= 0x40000000ULL && addr2 >= 0x40000000ULL)
                *(g_ram + (addr2 - 0x40000000ULL)) = virtio_blk_process(*hdr, sec, g_ram + (addr1 - 0x40000000ULL), len1);
        } else if (id == 2 && addr0 >= 0x40000000ULL) {
            virtio_snd_play_pcm(g_ram + (addr0 - 0x40000000ULL), len0);
        }
        uint16_t u_idx = *(used + 1);
        *(uint32_t *)(used + 2 + (u_idx % qsz) * 4) = head;
        *(uint32_t *)(used + 2 + (u_idx % qsz) * 4 + 2) = 0;
        *(used + 1) = u_idx + 1;
        l_idx++;
    }
    *(v_last_avail + id) = l_idx;
    gic_set_irq(48 + id, true);
}

uint64_t virtio_mmio_read(uint64_t addr, int size) {
    (void)size; int id = (int)((addr - 0x0a000000ULL) / 0x200ULL); uint64_t off = addr & 0x1FFULL;
    if (id < 0 || id >= 5 || !v_status) return 0;
    if (off == 0x000) return 0x74726976;
    if (off == 0x004) return 2;
    if (off == 0x008) return (id == 0) ? 2 : (id == 1 ? 1 : (id == 2 ? 25 : (id == 3 ? 16 : 18)));
    if (off == 0x034) return 128;
    if (off == 0x070) return *(v_status + id);
    if (off == 0x100 && id == 0) return virtio_blk_sectors();
    return 0;
}

void virtio_mmio_write(uint64_t addr, uint64_t val, int size) {
    (void)size; int id = (int)((addr - 0x0a000000ULL) / 0x200ULL); uint64_t off = addr & 0x1FFULL;
    if (id < 0 || id >= 5 || !v_status) return;
    if (off == 0x030) *(v_qsel + id) = (uint32_t)val;
    else if (off == 0x038) *(v_qnum + id) = (uint32_t)val;
    else if (off == 0x050) kick_vq(id);
    else if (off == 0x070) *(v_status + id) = (uint32_t)val;
    else if (off == 0x080) *(v_desc + id) = (*(v_desc + id) & ~0xFFFFFFFFULL) | val;
    else if (off == 0x084) *(v_desc + id) = (*(v_desc + id) & 0xFFFFFFFFULL) | (val << 32);
    else if (off == 0x090) *(v_avail + id) = (*(v_avail + id) & ~0xFFFFFFFFULL) | val;
    else if (off == 0x094) *(v_avail + id) = (*(v_avail + id) & 0xFFFFFFFFULL) | (val << 32);
    else if (off == 0x0a0) *(v_used + id) = (*(v_used + id) & ~0xFFFFFFFFULL) | val;
    else if (off == 0x0a4) *(v_used + id) = (*(v_used + id) & 0xFFFFFFFFULL) | (val << 32);
}

void virtio_push_input(uint16_t type, uint16_t code, int32_t value) {
    (void)type; (void)code; (void)value; gic_set_irq(52, true);
}
