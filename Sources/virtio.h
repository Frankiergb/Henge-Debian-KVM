#ifndef VIRTIO_H
#define VIRTIO_H

#include <stdint.h>
#include <stdbool.h>

void virtio_init(uint8_t *ram_base, uint64_t ram_size);
bool virtio_attach_disk(const char *img_path);
uint64_t virtio_mmio_read(uint64_t addr, int size);
void virtio_mmio_write(uint64_t addr, uint64_t val, int size);
void virtio_push_input(uint16_t type, uint16_t code, int32_t value);
const uint8_t *virtio_get_framebuffer(int *width, int *height);

#endif
