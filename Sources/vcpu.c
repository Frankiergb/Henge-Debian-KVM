#include "vcpu.h"
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <mach/mach.h>

typedef struct {
    uint64_t esr;
    uint64_t far;
    uint64_t pc;
    uint32_t cpsr;
    uint32_t pad;
    uint64_t x[29];
    uint64_t fp;
    uint64_t lr;
    uint64_t sp;
} ios_mcontext_t;

static uint8_t *guest_ram = NULL;
static uint64_t ram_size = 64 * 1024 * 1024;
static uart_tx_cb g_uart_cb = NULL;
static uint64_t v_vbar_el1 = 0, v_sctlr_el1 = 0, v_tcr_el1 = 0, v_ttbr0 = 0, v_ttbr1 = 0, v_hcr_el2 = 0x80000000;

static void handle_sigill(int sig, siginfo_t *info, void *uap) {
    struct __darwin_ucontext {
        int uc_onstack;
        uint32_t uc_sigmask;
        struct { void *ss_sp; size_t ss_size; int ss_flags; } uc_stack;
        void *uc_link;
        size_t uc_mcsize;
        ios_mcontext_t *uc_mcontext;
    } *uc = (struct __darwin_ucontext *)uap;

    ios_mcontext_t *mc = uc->uc_mcontext;
    uint32_t insn = *(uint32_t *)mc->pc;
    int rt = insn & 0x1F;

    if ((insn & 0xFFF00000) == 0xD5300000) {
        uint32_t sysreg = (insn >> 5) & 0x7FFF;
        uint64_t val = 0;
        if (sysreg == 0x4212) val = (2 << 2);      // CurrentEL = EL2 (0x8)
        else if (sysreg == 0x4020) val = 0x0011;   // ID_AA64PFR0_EL1 (支援 EL2)
        else if (sysreg == 0x4039) val = 0x000100; // ID_AA64MMFR1_EL1 (支援 VHE!)
        else if (sysreg == 0x4600) val = v_vbar_el1;
        else if (sysreg == 0x4080) val = v_sctlr_el1;
        else if (sysreg == 0x6088) val = v_hcr_el2;
        if (rt < 29) mc->x[rt] = val;
        else if (rt == 29) mc->fp = val;
        else if (rt == 30) mc->lr = val;
    } else if ((insn & 0xFFF00000) == 0xD5100000) {
        uint32_t sysreg = (insn >> 5) & 0x7FFF;
        uint64_t val = (rt < 29) ? mc->x[rt] : (rt == 29 ? mc->fp : (rt == 30 ? mc->lr : 0));
        if (sysreg == 0x4600) v_vbar_el1 = val;
        else if (sysreg == 0x4080) v_sctlr_el1 = val;
        else if (sysreg == 0x4100) v_ttbr0 = val;
        else if (sysreg == 0x4101) v_ttbr1 = val;
        else if (sysreg == 0x4102) v_tcr_el1 = val;
        else if (sysreg == 0x6088) v_hcr_el2 = val;
    }
    mc->pc += 4;
}

static void handle_sigsegv(int sig, siginfo_t *info, void *uap) {
    struct __darwin_ucontext {
        int uc_onstack;
        uint32_t uc_sigmask;
        struct { void *ss_sp; size_t ss_size; int ss_flags; } uc_stack;
        void *uc_link;
        size_t uc_mcsize;
        ios_mcontext_t *uc_mcontext;
    } *uc = (struct __darwin_ucontext *)uap;

    ios_mcontext_t *mc = uc->uc_mcontext;
    uint64_t addr = (uint64_t)info->si_addr;
    uint32_t insn = *(uint32_t *)mc->pc;
    int rt = insn & 0x1F;
    if ((addr & ~0xFFFULL) == 0x09000000 && g_uart_cb) {
        uint64_t val = (rt < 29) ? mc->x[rt] : (rt == 29 ? mc->fp : (rt == 30 ? mc->lr : 0));
        g_uart_cb((char)(val & 0xFF));
    }
    mc->pc += 4;
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
