#include "vcpu.h"
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <mach/arm/_structs.h>

static uint8_t *guest_ram = NULL;
static uint64_t ram_size = 64 * 1024 * 1024;
static uart_tx_cb g_uart_cb = NULL;
static uint64_t v_vbar_el1 = 0, v_sctlr_el1 = 0, v_tcr_el1 = 0, v_ttbr0 = 0, v_ttbr1 = 0, v_hcr_el2 = 0x80000000;

static void handle_sigill(int sig, siginfo_t *info, void *uap) {
    ucontext_t *uc = (ucontext_t *)uap;
    uint64_t pc = uc->uc_mcontext->__ss.__pc;
    uint32_t insn = *(uint32_t *)pc;
    int rt = insn & 0x1F;
    uint64_t *regs = uc->uc_mcontext->__ss.__x;

    if ((insn & 0xFFF00000) == 0xD5300000) {
        uint32_t sysreg = (insn >> 5) & 0x7FFF;
        uint64_t val = 0;
        if (sysreg == 0x4212) val = (2 << 2);      // CurrentEL = EL2 (0x8)
        else if (sysreg == 0x4020) val = 0x0011;   // ID_AA64PFR0_EL1 (支援 EL2)
        else if (sysreg == 0x4039) val = 0x000100; // ID_AA64MMFR1_EL1 (支援 VHE!)
        else if (sysreg == 0x4600) val = v_vbar_el1;
        else if (sysreg == 0x4080) val = v_sctlr_el1;
        else if (sysreg == 0x6088) val = v_hcr_el2;
        if (rt < 31) regs[rt] = val;
    } else if ((insn & 0xFFF00000) == 0xD5100000) {
        uint32_t sysreg = (insn >> 5) & 0x7FFF;
        uint64_t val = (rt < 31) ? regs[rt] : 0;
        if (sysreg == 0x4600) v_vbar_el1 = val;
        else if (sysreg == 0x4080) v_sctlr_el1 = val;
        else if (sysreg == 0x4100) v_ttbr0 = val;
        else if (sysreg == 0x4101) v_ttbr1 = val;
        else if (sysreg == 0x4102) v_tcr_el1 = val;
        else if (sysreg == 0x6088) v_hcr_el2 = val;
    }
    uc->uc_mcontext->__ss.__pc += 4;
}

static void handle_sigsegv(int sig, siginfo_t *info, void *uap) {
    ucontext_t *uc = (ucontext_t *)uap;
    uint64_t addr = (uint64_t)info->si_addr;
    uint32_t insn = *(uint32_t *)uc->uc_mcontext->__ss.__pc;
    int rt = insn & 0x1F;
    if ((addr & ~0xFFFULL) == 0x09000000 && g_uart_cb) {
        if (rt < 31) g_uart_cb((char)(uc->uc_mcontext->__ss.__x[rt] & 0xFF));
    }
    uc->uc_mcontext->__ss.__pc += 4;
}

bool vm_init_and_load(const uint8_t *kernel_data, uint64_t size, uart_tx_cb cb) {
    g_uart_cb = cb;
    if (guest_ram) munmap(guest_ram, ram_size);
    guest_ram = mmap((void *)0x40000000, ram_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (guest_ram == MAP_FAILED) return false;
    uint64_t copy_sz = size < ram_size ? size : ram_size;
    memcpy(guest_ram, kernel_data, copy_sz);

    struct sigaction sa_ill = { .sa_sigaction = handle_sigill, .sa_flags = SA_SIGINFO };
    sigaction(SIGILL, &sa_ill, NULL);
    struct sigaction sa_segv = { .sa_sigaction = handle_sigsegv, .sa_flags = SA_SIGINFO };
    sigaction(SIGSEGV, &sa_segv, NULL);
    return true;
}

void vm_run_vcpu(void) {
    if (!guest_ram) return;
    void (*entry)(uint64_t, uint64_t, uint64_t, uint64_t) = (void *)guest_ram;
    entry(0, 0, 0, 0);
}
