#include "gic_timer.h"
#include <string.h>
#include <mach/mach_time.h>

static uint32_t gicd_ctlr = 0;
static uint32_t irq_enabled[32];
static uint32_t irq_pending[32];
static uint32_t irq_active[32];

static uint64_t timer_ctl = 0;
static uint64_t timer_cval = 0;
static mach_timebase_info_data_t tb_info;

void gic_init(void) {
    gicd_ctlr = 0;
    memset(irq_enabled, 0, sizeof(irq_enabled));
    memset(irq_pending, 0, sizeof(irq_pending));
    memset(irq_active, 0, sizeof(irq_active));
    mach_timebase_info(&tb_info);
}

uint64_t gic_mmio_read(uint64_t offset, int size) {
    (void)size;
    if (offset == 0x0000) return gicd_ctlr;
    if (offset == 0x0004) return 0x0000001F; // 支援 1020 個中斷線
    if (offset == 0xFFE8) return 0x30;       // GICv3 PIDR2 架構版本標誌
    if (offset >= 0x0100 && offset < 0x0180) {
        return irq_enabled[(offset - 0x0100) / 4];
    }
    if (offset >= 0x0200 && offset < 0x0280) {
        return irq_pending[(offset - 0x0200) / 4];
    }
    return 0;
}

void gic_mmio_write(uint64_t offset, uint64_t val, int size) {
    (void)size;
    if (offset == 0x0000) {
        gicd_ctlr = (uint32_t)val;
    } else if (offset >= 0x0100 && offset < 0x0180) {
        irq_enabled[(offset - 0x0100) / 4] |= (uint32_t)val;
    } else if (offset >= 0x0180 && offset < 0x0200) {
        irq_enabled[(offset - 0x0180) / 4] &= ~(uint32_t)val;
    } else if (offset >= 0x0200 && offset < 0x0280) {
        irq_pending[(offset - 0x0200) / 4] |= (uint32_t)val;
    } else if (offset >= 0x0280 && offset < 0x0300) {
        irq_pending[(offset - 0x0280) / 4] &= ~(uint32_t)val;
    }
}

void gic_set_irq(uint32_t intid, bool level) {
    if (intid >= 1020) return;
    uint32_t idx = intid / 32, bit = 1U << (intid % 32);
    if (level) irq_pending[idx] |= bit;
    else irq_pending[idx] &= ~bit;
}

bool gic_has_pending_irq(void) {
    if ((gicd_ctlr & 3) == 0) return false;
    if (timer_check_pending()) gic_set_irq(27, true); // Virtual Timer PPI 27
    for (int i = 0; i < 32; i++) {
        if ((irq_pending[i] & irq_enabled[i] & ~irq_active[i]) != 0) return true;
    }
    return false;
}

uint32_t gic_ack_irq(void) {
    for (int i = 0; i < 32; i++) {
        uint32_t mask = irq_pending[i] & irq_enabled[i] & ~irq_active[i];
        if (mask) {
            int bit = __builtin_ctz(mask);
            irq_active[i] |= (1U << bit);
            irq_pending[i] &= ~(1U << bit);
            return (uint32_t)(i * 32 + bit);
        }
    }
    return 1023; // Spurious IRQ
}

void gic_eoi_irq(uint32_t intid) {
    if (intid >= 1020) return;
    irq_active[intid / 32] &= ~(1U << (intid % 32));
}

uint64_t timer_get_cntpct(void) {
    uint64_t ns = mach_absolute_time() * tb_info.numer / tb_info.denom;
    return (ns * 24) / 1000; // 模擬標準 24MHz ARM Generic Timer
}

void timer_write_ctl(uint64_t val) { timer_ctl = val; }
void timer_write_cval(uint64_t val) { timer_cval = val; }

bool timer_check_pending(void) {
    if ((timer_ctl & 1) == 0 || (timer_ctl & 2) != 0) return false;
    return timer_get_cntpct() >= timer_cval;
}
