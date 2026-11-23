#include "vcpu.h"
#include "dtb_mmu.h"
#include "gic_timer.h"
#include "virtio.h"
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <mach/mach.h>

static uint8_t *guest_ram = NULL;
static uint64_t ram_size = 256 * 1024 * 1024, dtb_gpa = 0;
static uart_tx_cb g_uart_cb = NULL;
static uint64_t v_sctlr = 0, v_ttbr0 = 0, v_ttbr1 = 0, v_hcr = 0x80000000, v_vttbr = 0, v_elr_el2 = 0, v_spsr_el2 = 0;
static bool in_l2_guest = false;

static void handle_sigill(int sig, siginfo_t *info, void *uap) {
    (void)sig; (void)info;
    struct { int a; uint32_t b; struct { void *p; size_t s; int f; } c; void *d; size_t e; arm_thread_state64_t *mc; } *uc = uap;
    arm_thread_state64_t *ts = (arm_thread_state64_t *)((char *)uc->mc + 16);
    uint64_t pc = arm_thread_state64_get_pc(*ts);
    uint32_t insn = *(uint32_t *)pc;
    int rt = insn & 0x1F;
    uint32_t sysreg = (insn >> 5) & 0x7FFF;

    if (insn == 0xD69F03E0) { // ERET 指令：Debian KVM 啟動第二層虛擬機 (L2 Guest)！
        in_l2_guest = true;
        arm_thread_state64_set_pc_fptr(*ts, (void *)v_elr_el2);
        return;
    } else if ((insn & 0xFFF00000) == 0xD5300000) { // MRS
        uint64_t val = 0;
        if (sysreg == 0x4212) val = in_l2_guest ? (1 << 2) : (2 << 2); // EL2 (Host) / EL1 (L2 Guest)
        else if (sysreg == 0x4000) val = 0x410FD083; // MIDR_EL1
        else if (sysreg == 0x4001) val = 0x8444C004; // CTR_EL0 (64B cache line)
        else if (sysreg == 0x4020) val = 0x0011;     // ID_AA64PFR0_EL1 (EL2)
        else if (sysreg == 0x4039) val = 0x000100;   // ID_AA64MMFR1_EL1 (VHE!)
        else if (sysreg == 0x4080) val = v_sctlr;
        else if (sysreg == 0x6088) val = v_hcr;
        else if (sysreg == 0x6108) val = v_vttbr;
        else if (sysreg == 0x6201) val = v_elr_el2;
        else if (sysreg == 0x5F01) val = timer_get_cntpct();
        if (rt < 29) *(&ts->__x + rt) = val;
    } else if ((insn & 0xFFF00000) == 0xD5100000) { // MSR
        uint64_t val = (rt < 29) ? *(&ts->__x + rt) : 0;
        if (sysreg == 0x4080) { v_sctlr = val; if (val & 1) mmu_sync_mappings(guest_ram, ram_size, v_ttbr0, v_ttbr1); }
        else if (sysreg == 0x4100) v_ttbr0 = val;
        else if (sysreg == 0x4101) v_ttbr1 = val;
        else if (sysreg == 0x6088) v_hcr = val;
        else if (sysreg == 0x6108) v_vttbr = val;
        else if (sysreg == 0x6200) v_spsr_el2 = val;
        else if (sysreg == 0x6201) v_elr_el2 = val;
        else if (sysreg == 0x5F19) timer_write_ctl(val);
        else if (sysreg == 0x5F1A) timer_write_cval(val);
    }
    arm_thread_state64_set_pc_fptr(*ts, (void *)(pc + 4));
}

static void handle_sigsegv(int sig, siginfo_t *info, void *uap) {
    (void)sig;
    struct { int a; uint32_t b; struct { void *p; size_t s; int f; } c; void *d; size_t e; arm_thread_state64_t *mc; } *uc = uap;
    arm_thread_state64_t *ts = (arm_thread_state64_t *)((char *)uc->mc + 16);
    uint64_t pc = arm_thread_state64_get_pc(*ts);
    uint64_t addr = (uint64_t)info->si_addr;
    uint32_t insn = *(uint32_t *)pc;
    int rt = insn & 0x1F;
    bool is_write = ((insn >> 22) & 1) == 0;
    uint64_t val = (rt < 29) ? *(&ts->__x + rt) : 0;

    if ((addr & ~0xFFFULL) == 0x09000000 && is_write && g_uart_cb) g_uart_cb((char)(val & 0xFF));
    else if (addr >= 0x08000000ULL && addr < 0x08200000ULL) {
        if (is_write) gic_mmio_write(addr & 0xFFFFULL, val, 4);
        else if (rt < 29) *(&ts->__x + rt) = gic_mmio_read(addr & 0xFFFFULL, 4);
    } else if (addr >= 0x0a000000ULL && addr < 0x0a001000ULL) {
        if (is_write) virtio_mmio_write(addr, val, 4);
        else if (rt < 29) *(&ts->__x + rt) = virtio_mmio_read(addr, 4);
    } else if (addr >= 0xffff000000000000ULL) { // 高位虛擬頁按需動態映射
        uint64_t page = addr & ~0xFFFULL;
        uint64_t pa = mmu_translate_va(guest_ram, ram_size, page, v_ttbr0, v_ttbr1, v_vttbr, in_l2_guest);
        if (pa >= 0x40000000ULL && pa < 0x40000000ULL + ram_size) {
            void *p = mmap((void *)page, 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
            if (p != MAP_FAILED) { memcpy(p, guest_ram + (pa - 0x40000000ULL), 0x1000); return; }
        }
    }
    arm_thread_state64_set_pc_fptr(*ts, (void *)(pc + 4));
}

bool vm_init_and_load(const uint8_t *kernel_data, uint64_t size, const char *disk_path, uart_tx_cb cb) {
    g_uart_cb = cb; in_l2_guest = false;
    if (guest_ram) munmap(guest_ram, ram_size);
    guest_ram = mmap((void *)0x40000000, ram_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (guest_ram == MAP_FAILED) return false;
    memcpy(guest_ram, kernel_data, size < (ram_size - 0x100000) ? size : (ram_size - 0x100000));
    dtb_gpa = build_arm64_dtb(guest_ram, ram_size, ram_size - 0x100000);
    gic_init(); virtio_init(guest_ram, ram_size);
    if (disk_path && *disk_path != '\0') virtio_attach_disk(disk_path);

    struct sigaction sa1 = { .sa_sigaction = handle_sigill, .sa_flags = SA_SIGINFO }; sigaction(SIGILL, &sa1, NULL);
    struct sigaction sa2 = { .sa_sigaction = handle_sigsegv, .sa_flags = SA_SIGINFO }; sigaction(SIGSEGV, &sa2, NULL);
    return true;
}

void vm_run_vcpu(void) {
    if (!guest_ram) return;
    void (*entry)(uint64_t, uint64_t, uint64_t, uint64_t) = (void *)guest_ram;
    entry(dtb_gpa, 0, 0, 0);
}
