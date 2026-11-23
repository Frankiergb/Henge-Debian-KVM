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

extern boolean_t mach_exc_server(mach_msg_header_t *InHeadP, mach_msg_header_t *OutHeadP);

kern_return_t catch_mach_exception_raise(
    mach_port_t exception_port, mach_port_t thread, mach_port_t task,
    exception_type_t exception, mach_exception_data_t code, mach_msg_type_number_t codeCnt) {
    (void)exception_port; (void)task; (void)codeCnt;
    arm_thread_state64_t ts;
    mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(thread, ARM_THREAD_STATE64, (thread_state_t)&ts, &cnt) != KERN_SUCCESS) return KERN_FAILURE;

    uint64_t pc = (uint64_t)arm_thread_state64_get_pc(ts);
    if (pc < guest_base_rx || pc >= guest_base_rx + ram_size) return KERN_FAILURE;

    uint32_t insn = *(uint32_t *)pc;
    int rt = insn & 0x1F;
    uint64_t *regs = ts.__x;

    if (exception == EXC_BAD_INSTRUCTION) {
        uint32_t sysreg = (insn >> 5) & 0x7FFF;
        if ((insn & 0xFFF00000) == 0xD5300000) {
            uint64_t val = 0;
            if (sysreg == 0x4212) val = (2 << 2);      // CurrentEL = EL2 (0x8)
            else if (sysreg == 0x4020) val = 0x0011;   // EL2 support
            else if (sysreg == 0x4039) val = 0x000100; // VHE support
            else if (sysreg == 0x5F01) val = timer_get_cntpct();
            if (rt < 29) *(regs + rt) = val;
        }
    } else if (exception == EXC_BAD_ACCESS) {
        uint64_t fault_addr = (uint64_t)*(code + 1);
        bool is_write = ((insn >> 22) & 1) == 0;
        uint64_t val = (rt = 2 && *kernel_data == 0x1F && *(kernel_data + 1) == 0x8B) {
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
    thread_set_exception_ports(mach_thread_self(), EXC_MASK_BAD_INSTRUCTION | EXC_MASK_BAD_ACCESS, exc_port, EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES, ARM_THREAD_STATE64);

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
