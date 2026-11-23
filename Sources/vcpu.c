#include "vcpu.h"
#include "dtb_mmu.h"
#include "gic_timer.h"
#include "virtio.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <zlib.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <libkern/OSCacheControl.h>

static uint8_t *rw_ram = NULL, *rx_ram = NULL;
static uint64_t ram_size = 128 * 1024 * 1024, dtb_gpa = 0, guest_base_rx = 0;
static uart_tx_cb g_uart_cb = NULL;
static uint64_t v_sctlr = 0, v_ttbr0 = 0, v_ttbr1 = 0, v_hcr = 0x80000000, v_vttbr = 0, v_elr_el2 = 0;
static bool in_l2_guest = false;
static mach_port_t exc_port = MACH_PORT_NULL;

static void log_msg(const char *s) { if (g_uart_cb) while (*s) g_uart_cb(*s++); }

static void handle_guest_trap(arm_thread_state64_t *ts, uint64_t fault_addr, bool is_insn_fault) {
    uint64_t pc = (uint64_t)arm_thread_state64_get_pc(*ts);
    if (pc < guest_base_rx || pc >= guest_base_rx + ram_size) return;
    uint32_t insn = *(uint32_t *)pc;
    int rt = insn & 0x1F;
    uint64_t *regs = ts->__x;

    if (is_insn_fault) {
        uint32_t sysreg = (insn >> 5) & 0x7FFF;
        if (insn == 0xD69F03E0) {
            in_l2_guest = true;
            arm_thread_state64_set_pc_fptr(*ts, (void *)v_elr_el2);
            return;
        } else if ((insn & 0xFFF00000) == 0xD5300000) {
            uint64_t val = 0;
            if (sysreg == 0x4212) val = in_l2_guest ? (1 << 2) : (2 << 2);
            else if (sysreg == 0x4000) val = 0x410FD083;
            else if (sysreg == 0x4001) val = 0x8444C004;
            else if (sysreg == 0x4020) val = 0x0011;
            else if (sysreg == 0x4039) val = 0x000100;
            else if (sysreg == 0x4080) val = v_sctlr;
            else if (sysreg == 0x6088) val = v_hcr;
            else if (sysreg == 0x6108) val = v_vttbr;
            else if (sysreg == 0x5F01) val = timer_get_cntpct();
            if (rt < 29) regs[rt] = val;
        } else if ((insn & 0xFFF00000) == 0xD5100000) {
            uint64_t val = (rt < 29) ? regs[rt] : 0;
            if (sysreg == 0x4080) v_sctlr = val;
            else if (sysreg == 0x4100) v_ttbr0 = val;
            else if (sysreg == 0x4101) v_ttbr1 = val;
            else if (sysreg == 0x6088) v_hcr = val;
            else if (sysreg == 0x6108) v_vttbr = val;
            else if (sysreg == 0x6201) v_elr_el2 = val;
            else if (sysreg == 0x5F19) timer_write_ctl(val);
            else if (sysreg == 0x5F1A) timer_write_cval(val);
        }
    } else {
        bool is_write = ((insn >> 22) & 1) == 0;
        uint64_t val = (rt < 29) ? regs[rt] : 0;
        if ((fault_addr & ~0xFFFULL) == 0x09000000 && is_write && g_uart_cb) {
            g_uart_cb((char)(val & 0xFF));
        } else if (fault_addr >= 0x08000000ULL && fault_addr < 0x08200000ULL) {
            if (is_write) gic_mmio_write(fault_addr & 0xFFFFULL, val, 4);
            else if (rt < 29) regs[rt] = gic_mmio_read(fault_addr & 0xFFFFULL, 4);
        } else if (fault_addr >= 0x0a000000ULL && fault_addr < 0x0a001000ULL) {
            if (is_write) virtio_mmio_write(fault_addr, val, 4);
            else if (rt < 29) regs[rt] = virtio_mmio_read(fault_addr, 4);
        }
    }
    arm_thread_state64_set_pc_fptr(*ts, (void *)(pc + 4));
}

static void *mach_exc_loop(void *arg) {
    (void)arg;
    uint8_t buf[1024];
    mach_msg_header_t *hdr = (mach_msg_header_t *)buf;
    while (1) {
        if (mach_msg(hdr, MACH_RCV_MSG, 0, sizeof(buf), exc_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL) != KERN_SUCCESS) continue;
        uint8_t *body = buf + sizeof(mach_msg_header_t) + sizeof(NDR_record_t);
        exception_type_t exc = *(exception_type_t *)body;
        int64_t *codes = (int64_t *)(body + 8);
        thread_act_t thr = hdr->msgh_remote_port;
        arm_thread_state64_t ts;
        mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
        if (thread_get_state(thr, ARM_THREAD_STATE64, (thread_state_t)&ts, &cnt) == KERN_SUCCESS) {
            handle_guest_trap(&ts, (uint64_t)codes[1], exc == EXC_BAD_INSTRUCTION);
            thread_set_state(thr, ARM_THREAD_STATE64, (thread_state_t)&ts, ARM_THREAD_STATE64_COUNT);
        }
        struct { mach_msg_header_t Head; NDR_record_t NDR; kern_return_t RetCode; } rep = {
            .Head = { .msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(hdr->msgh_bits), 0), .msgh_size = sizeof(rep), .msgh_remote_port = thr, .msgh_id = hdr->msgh_id + 100 },
            .NDR = NDR_record, .RetCode = KERN_SUCCESS
        };
        mach_msg(&rep.Head, MACH_SEND_MSG, sizeof(rep), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    }
    return NULL;
}

bool vm_init_and_load(const uint8_t *kernel_data, uint64_t size, const char *disk_path, uart_tx_cb cb) {
    g_uart_cb = cb; in_l2_guest = false;
    if (!rw_ram) {
        vm_address_t rw = 0, rx = 0;
        if (vm_allocate(mach_task_self(), &rw, ram_size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) { log_msg("[錯誤] vm_allocate 失敗\n"); return false; }
        vm_prot_t cur, max;
        if (vm_remap(mach_task_self(), &rx, ram_size, 0, VM_FLAGS_ANYWHERE, mach_task_self(), rw, FALSE, &cur, &max, VM_INHERIT_NONE) != KERN_SUCCESS) { log_msg("[錯誤] vm_remap 失敗\n"); return false; }
        if (vm_protect(mach_task_self(), rx, ram_size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE) != KERN_SUCCESS) { log_msg("[錯誤] vm_protect RX 失敗\n"); return false; }
        rw_ram = (uint8_t *)rw; rx_ram = (uint8_t *)rx; guest_base_rx = (uint64_t)rx;
    }
    memset(rw_ram, 0, ram_size);
    if (size >= 2 && kernel_data[0] == 0x1F && kernel_data[1] == 0x8B) {
        log_msg("[解壓] 檢測到 GZIP 壓縮內核，正在解壓...\n");
        z_stream strm = { .next_in = (Bytef *)kernel_data, .avail_in = (uInt)size, .next_out = rw_ram, .avail_out = (uInt)(ram_size - 0x100000) };
        if (inflateInit2(&strm, 16 + MAX_WBITS) == Z_OK) { inflate(&strm, Z_FINISH); inflateEnd(&strm); }
    } else {
        memcpy(rw_ram, kernel_data, size < (ram_size - 0x100000) ? size : (ram_size - 0x100000));
    }
    if (*(uint32_t *)(rw_ram + 56) != 0x644d5241) {
        log_msg("[警告] 非標準 ARM64 Image 檔頭（請確認匯入未壓縮或 GZIP 嘅 arm64 Image），已攔截防閃退。\n");
        return false;
    }
    sys_icache_invalidate(rx_ram, ram_size);
    dtb_gpa = build_arm64_dtb(rw_ram, ram_size, ram_size - 0x100000);
    gic_init(); virtio_init(rw_ram, ram_size);
    if (disk_path && disk_path[0] != '\0') virtio_attach_disk(disk_path);
    return true;
}

void vm_run_vcpu(void) {
    if (!rx_ram) return;
    if (exc_port == MACH_PORT_NULL) {
        mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &exc_port);
        mach_port_insert_right(mach_task_self(), exc_port, exc_port, MACH_MSG_TYPE_MAKE_SEND);
        pthread_t th; pthread_create(&th, NULL, mach_exc_loop, NULL);
    }
    thread_set_exception_ports(mach_thread_self(), EXC_MASK_BAD_INSTRUCTION | EXC_MASK_BAD_ACCESS, exc_port, EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES, ARM_THREAD_STATE64);
    uint64_t entry = guest_base_rx, dtb = dtb_gpa;
    __asm__ volatile("mov x0, %0\n mov x1, xzr\n mov x2, xzr\n mov x3, xzr\n br %1\n" : : "r"(dtb), "r"(entry) : "x0", "x1", "x2", "x3");
}
