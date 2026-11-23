#ifndef VCPU_H
#define VCPU_H

#include <stdint.h>
#include <stdbool.h>

typedef void (*uart_tx_cb)(char c);

bool vm_init_and_load(const uint8_t *kernel_data, uint64_t size, uart_tx_cb cb);
void vm_run_vcpu(void);

#endif
