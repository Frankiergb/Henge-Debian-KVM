#ifndef GIC_TIMER_H
#define GIC_TIMER_H

#include <stdint.h>
#include <stdbool.h>

void gic_init(void);
uint64_t gic_mmio_read(uint64_t offset, int size);
void gic_mmio_write(uint64_t offset, uint64_t val, int size);
void gic_set_irq(uint32_t intid, bool level);
uint32_t gic_ack_irq(void);
void gic_eoi_irq(uint32_t intid);
bool gic_has_pending_irq(void);

uint64_t timer_get_cntpct(void);
void timer_write_ctl(uint64_t val);
void timer_write_cval(uint64_t val);
bool timer_check_pending(void);

#endif
