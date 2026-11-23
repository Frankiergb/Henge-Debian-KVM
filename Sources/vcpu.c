#include "vcpu.h"
#include "dtb_mmu.h"
#include "gic_timer.h"
#include "virtio.h"
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

static uint8_t *rw_ram = NULL, *rx_ram = NULL;
static uint64_t ram_size = 128 * 1024 * 1024, dtb_gpa = 0;
static uart_tx_cb g_uart_cb = NULL;
static uint64_t v_sctlr = 0, v_ttbr0 = 0, v_ttbr1 = 0, v_hcr = 0x80000000, v_vttbr = 0, v_elr_el2 = 0;
static bool in_l2_guest = false;

static void handle_sigill(int sig, siginfo_t *info, void *uap) {
    (void)sig; (void)info;
    ucontext_t *uc = (ucontext_t *)uap;
    arm_thread_state64_t *ts = &uc->uc_mcontext->__ss;
    uint64_t pc = arm_thread_state64_get_pc(*ts);
    uint32_t insn = *(uint32_t *)pc;
    int rt = insn & 0x1F;
    uint32_t sysreg = (insn >> 5) & 0x7FFF;

    if (insn == 0xD69F03E0) { in_l2_guest = true; arm_thread_state64_set_pc_fptr(*ts, (void *)v_elr_el2); return; }
    else if ((insn & 0xFFF00000) == 0xD5300000) {
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
        if (rt < 29) ts->__x[rt] = val;
    } else if ((insn & 0xFFF00000) == 0xD5100000) {
        uint64_t val = (rt < 29) ? ts->__x[rt] : 0;
        if (sysreg == 0x4080) v_sctlr = val;
        else if (sysreg == 0x4100) v_ttbr0 = val;
        else if (sysreg == 0x4101) v_ttbr1 = val;
        else if (sysreg == 0x6088) v_hcr = val;
        else if (sysreg == 0x6108) v_vttbr = val;
        else if (sysreg == 0x6201) v_elr_el2 = val;
        else if (sysreg == 0x5F19) timer_write_ctl(val);
        else if (sysreg == 0x5F1A) timer_write_cval(val);
    }
    arm_thread_state64_set_pc_fptr(*ts, (void *)(pc + 4));
}

static void handle_fault(int sig, siginfo_t *info, void *uap) {
    (void)sig;
    ucontext_t *uc = (ucontext_t *)uap;
    arm_thread_state64_t *ts = &uc->uc_mcontext->__ss;
    uint64_t pc = arm_thread_state64_get_pc(*ts);
    uint64_t addr = (uint64_t)info->si_addr;
    uint32_t insn = *(uint32_t *)pc;
    int rt = insn & 0x1F;
    bool is_write = ((insn >> 22) & 1) == 0;
    uint64_t val = (rt < 29) ? ts->__x[rt] : 0;

    if ((addr & ~0xFFFULL) == 0x09000000 && is_write && g_uart_cb) g_uart_cb((char)(val & 0xFF));
    else if (addr >= 0x08000000ULL && addr < 0x08200000ULL) {
        if (is_write) gic_mmio_write(addr & 0xFFFFULL, val, 4);
        else if (rt < 29) ts->__x[rt] = gic_mmio_read(addr & 0xFFFFULL, 4);
    } else if (addr >= 0x0a000000ULL && addr < 0x0a001000ULL) {
        if (is_write) virtio_mmio_write(addr, val, 4);
        else if (rt < 29) ts->__x[rt] = virtio_mmio_read(addr, 4);
    }
    arm_thread_state64_set_pc_fptr(*ts, (void *)(pc + 4));
}

bool vm_init_and_load(const uint8_t *kernel_data, uint64_t size, const char *disk_path, uart_tx_cb cb) {
    g_uart_cb = cb; in_l2_guest = false;
    if (!rw_ram) {
        mach_vm_address_t rw = 0, rx = 0;
        if (mach_vm_allocate(mach_task_self(), &rw, ram_size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) return false;
        vm_prot_t cur, max;
        if (mach_vm_remap(mach_task_self(), &rx, ram_size, 0, VM_FLAGS_ANYWHERE, mach_task_self(), rw, FALSE, &cur, &max, VM_INHERIT_NONE) != KERN_SUCCESS) return false;
        mach_vm_protect(mach_task_self(), rx, ram_size, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
        rw_ram = (uint8_t *)rw; rx_ram = (uint8_t *)rx;
    }
    memset(rw_ram, 0, ram_size);
    memcpy(rw_ram, kernel_data, size < (ram_size - 0x100000) ? size : (ram_size - 0x100000));
    dtb_gpa = build_arm64_dtb(rw_ram, ram_size, ram_size - 0x100000);
    gic_init(); virtio_init(rw_ram, ram_size);
    if (disk_path && *disk_path != '\0') virtio_attach_disk(disk_path);

    struct sigaction sa1 = { .sa_sigaction = handle_sigill, .sa_flags = SA_SIGINFO }; sigaction(SIGILL, &sa1, NULL);
    struct sigaction sa2 = { .sa_sigaction = handle_fault, .sa_flags = SA_SIGINFO };
    sigaction(SIGSEGV, &sa2, NULL); sigaction(SIGBUS, &sa2, NULL);
    return true;
}

void vm_run_vcpu(void) {
    if (!rx_ram) return;
    void (*entry)(uint64_t, uint64_t, uint64_t, uint64_t) = (void *)rx_ram;
    entry(dtb_gpa, 0, 0, 0);
}
