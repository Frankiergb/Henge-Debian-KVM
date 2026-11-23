#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static int disk_fd = -1;
static uint64_t disk_sectors = 0;

bool virtio_blk_open(const char *path) {
    if (disk_fd >= 0) close(disk_fd);
    disk_fd = open(path, O_RDWR);
    if (disk_fd < 0) return false;
    off_t sz = lseek(disk_fd, 0, SEEK_END);
    disk_sectors = (sz > 0) ? ((uint64_t)sz / 512ULL) : 0;
    return true;
}

uint64_t virtio_blk_sectors(void) { return disk_sectors; }

uint8_t virtio_blk_process(uint32_t type, uint64_t sector, uint8_t *buf, uint32_t len) {
    if (disk_fd < 0) return 1; // VIRTIO_BLK_S_IOERR
    if (type == 0) { // READ
        return (pread(disk_fd, buf, len, (off_t)(sector * 512ULL)) == (ssize_t)len) ? 0 : 1;
    } else if (type == 1) { // WRITE
        return (pwrite(disk_fd, buf, len, (off_t)(sector * 512ULL)) == (ssize_t)len) ? 0 : 1;
    }
    return 0; // FLUSH / OK
}

void virtio_net_tx(const uint8_t *frame, uint32_t len) {
    // 預留用戶態 NAT / DHCP 回應封包注入接口
    (void)frame; (void)len;
}
