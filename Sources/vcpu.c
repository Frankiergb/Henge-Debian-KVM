#include "vcpu.h"
#include "dtb_mmu.h"
#include "gic_timer.h"
#include "virtio.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <zlib.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <libkern/OSCacheControl.h>

static uint8_t *rw_ram = NULL, *rx_ram = NULL;
static uint64_t ram_size = 64 * 1024 * 1024, dtb_gpa = 0, guest_base_rx = 0;
static uart_tx_cb g_uart_cb = NULL;
static mach_port_t exc_port = MACH_PORT_NULL;

static void log_msg(const char *s) { if (g_uart_cb) while (*s) g_uart_cb(*s++); }

static void *exc_server_thread(void *arg) {
    (void)arg;
    uint8_t *buf = (uint8_t *)malloc(2048);
    mach_msg_header_t *hdr = (mach_msg_header_t *)buf;
    while (1) {
        if (mach_msg(hdr, MACH_RCV_MSG, 0, 2048, exc_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL) != KERN_SUCCESS) continue;
        mach_port_t thread = hdr->msgh_remote_port;
        arm_thread_state64_t ts;
        mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
        if (thread_get_state(thread, ARM_THREAD_STATE64, (thread_state_t)&ts, &cnt) == KERN_SUCCESS) {
            uint64_t pc = (uint64_t)arm_thread_state64_get_pc(ts);
            if (pc >= guest_base_rx && pc < guest_base_rx + ram_size) {
                uint32_t insn = *(uint32_t *)pc;
                int rt = insn & 0x1F;
                uint64_t *regs = ts.__x;
                if ((insn & 0xFFF00000) == 0xD5300000) {
                    uint32_t sysreg = (insn >> 5) & 0x7FFF;
                    uint64_t val = (sysreg == 0x4212) ? (2 << 2) : 0;
                    if (rt < 29) *(regs + rt) = val;
                }
                arm_thread_state64_set_pc_fptr(ts, (void *)(pc + 4));
                thread_set_state(thread, ARM_THREAD_STATE64, (thread_state_t)&ts, ARM_THREAD_STATE64_COUNT);
            }
        }
        struct { mach_msg_header_t Head; NDR_record_t NDR; kern_return_t RetCode; } rep = {
            .Head = { .msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(hdr->msgh_bits), 0), .msgh_size = sizeof(rep), .msgh_remote_port = thread, .msgh_id = hdr->msgh_id + 100 },
            .NDR = NDR_record, .RetCode = KERN_SUCCESS
        };
        mach_msg(&rep.Head, MACH_SEND_MSG, sizeof(rep), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    }
    return NULL;
}

bool vm_init_and_load(const uint8_t *kernel_data, uint64_t size, const char *disk_path, uart_tx_cb cb) {
    g_uart_cb = cb;
    log_msg("[步驟1] 申請雙重映射 JIT 記憶體...\n");
    if (!rw_ram) {
        vm_address_t rw = 0, rx = 0;
        if (vm_allocate(mach_task_self(), &rw, ram_size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) { log_msg("[失敗] vm_allocate 失敗\n"); return false; }
        vm_prot_t cur, max;
        if (vm_remap(mach_task_self(), &rx, ram_size, 0, VM_FLAGS_ANYWHERE, mach_task_self(), rw, FALSE, &cur, &max, VM_INHERIT_NONE) != KERN_SUCCESS) { log_msg("[失敗] vm_remap 失敗\n"); return false; }
        if (vm_protect(mach_task_self(), rx, ram_size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE) != KERN_SUCCESS) { log_msg("[失敗] vm_protect RX 失敗（iOS 禁止執行權限）\n"); return false; }
        rw_ram = (uint8_t *)rw; rx_ram = (uint8_t *)rx; guest_base_rx = (uint64_t)rx;
    }
    log_msg("[步驟2] 雙重映射 RW/RX 成功！載入內核...\n");
    memset(rw_ram, 0, ram_size);
    if (size >= 2 && *kernel_data == 0x1F && *(kernel_data + 1) == 0x8B) {
        z_stream strm = { .next_in = (Bytef *)kernel_data, .avail_in = (uInt)size, .next_out = rw_ram, .avail_out = (uInt)(ram_size - 0x100000) };
        if (inflateInit2(&strm, 16 + MAX_WBITS) == Z_OK) { inflate(&strm, Z_FINISH); inflateEnd(&strm); }
    } else {
        memcpy(rw_ram, kernel_data, size < (ram_size - 0x100000) ? size : (ram_size - 0x100000));
    }
    sys_icache_invalidate(rx_ram, ram_size);
    dtb_gpa = build_arm64_dtb(rw_ram, ram_size, ram_size - 0x100000);
    gic_init(); virtio_init(rw_ram, ram_size);
    if (disk_path && *disk_path != '\0') virtio_attach_disk(disk_path);
    return true;
}

void vm_run_vcpu(void) {
    if (!rx_ram) return;
    if (exc_port == MACH_PORT_NULL) {
        mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &exc_port);
        mach_port_insert_right(mach_task_self(), exc_port, exc_port, MACH_MSG_TYPE_MAKE_SEND);
        pthread_t th; pthread_create(&th, NULL, exc_server_thread, NULL);
    }
    thread_set_exception_ports(mach_thread_self(), EXC_MASK_BAD_INSTRUCTION | EXC_MASK_BAD_ACCESS, exc_port, EXCEPTION_DEFAULT, ARM_THREAD_STATE64);

    uint32_t *test_rw = (uint32_t *)(rw_ram + ram_size - 0x2000);
    uint64_t test_rx = guest_base_rx + ram_size - 0x2000;
    *test_rw = 0xD5384240;       // MRS X0, CurrentEL
    *(test_rw + 1) = 0xD65F03C0; // RET
    sys_icache_invalidate((void *)test_rx, 16);

    log_msg("[步驟3] 正在實測原生執行 MRS CurrentEL 並攔截偽裝 EL2...\n");
    uint64_t el_res = 0;
    __asm__ volatile("blr %1\n mov %0, x0\n" : "=r"(el_res) : "r"(test_rx) : "x0", "x30");
    char *msg = (char *)malloc(256);
    if (msg) {
        snprintf(msg, 256, "[成功!] 原生機器碼已返回！攔截結果 CurrentEL = 0x%llx (EL%llu)\n", el_res, el_res >> 2);
        log_msg(msg);
        free(msg);
    }
}
