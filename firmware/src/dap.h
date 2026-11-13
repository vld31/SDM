// Universal Black Box - ADIv5/ADIv6 debug port + memory access port layer
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    bool     adiv6;            // RP2350: ADIv6 DP, APs addressed by base address
    uint32_t targetsel[2];     // per-core TARGETSEL values (ADIv5 multidrop); [1]=0 if single DP
    uint32_t ap_base[2];       // ADIv5: APSEL<<24 ; ADIv6: AP base address (e.g. 0x2000)
    uint32_t timerawl_addr;    // free-running target microsecond counter (no read side effects)
    uint32_t chip_reset_addr;  // reset-cause register
    uint32_t wdog_reason_addr; // watchdog reason register
    uint32_t sram_base, sram_size;
} dap_profile_t;

// DP register addresses
#define DP_DPIDR    0x0
#define DP_ABORT    0x0
#define DP_CTRLSTAT 0x4
#define DP_SELECT   0x8
#define DP_RDBUFF   0xC
#define DP_TARGETSEL 0xC

// AP registers (offset inside the AP, bank in bits [7:4])
#define AP_CSW  0x00
#define AP_TAR  0x04
#define AP_DRW  0x0C
#define AP_IDR  0xFC

// SCS / debug registers
#define SCS_DHCSR 0xE000EDF0u
#define SCS_DCRSR 0xE000EDF4u
#define SCS_DCRDR 0xE000EDF8u
#define SCS_DEMCR 0xE000EDFCu
#define SCS_DFSR  0xE000ED30u
#define SCS_ICSR  0xE000ED04u
#define SCS_AIRCR 0xE000ED0Cu
#define SCS_CPUID 0xE000ED00u
#define DWT_PCSR  0xE000101Cu

#define DHCSR_KEY        0xA05F0000u
#define DHCSR_C_DEBUGEN  (1u << 0)
#define DHCSR_C_HALT     (1u << 1)
#define DHCSR_C_STEP     (1u << 2)
#define DHCSR_C_MASKINTS (1u << 3)
#define DHCSR_S_REGRDY   (1u << 16)
#define DHCSR_S_HALT     (1u << 17)
#define DHCSR_S_SLEEP    (1u << 18)
#define DHCSR_S_LOCKUP   (1u << 19)
#define DHCSR_S_RETIRE_ST (1u << 24)
#define DHCSR_S_RESET_ST (1u << 25)

#define DEMCR_VC_CORERESET (1u << 0)
#define DEMCR_VC_HARDERR   (1u << 10)
#define DEMCR_DWTENA       (1u << 24)

#define DAP_OK          0
#define DAP_ERR_SWD     1
#define DAP_ERR_NOTARGET 2
#define DAP_ERR_TIMEOUT 3
#define DAP_ERR_POWER   4

void dap_set_profile(const dap_profile_t *p);
const dap_profile_t *dap_profile(void);

// Full connect: preamble sequences, select core 0 DP, read DPIDR, power up, init AP.
int  dap_connect(uint32_t *dpidr_out);
// Select which core's DP/AP subsequent accesses go through.  On RP2040 this
// re-selects the multidrop DP (line reset + TARGETSEL); on ADIv6 it only
// changes the AP base.
int  dap_select_core(int core);
int  dap_current_core(void);
void dap_invalidate(void);   // forget cached SELECT/CSW/TAR (after any line reset)

int  dap_dp_read(uint8_t addr, uint32_t *v);
int  dap_dp_write(uint8_t addr, uint32_t v);
int  dap_clear_errors(void);

int  dap_mem_read32(uint32_t addr, uint32_t *v);
int  dap_mem_write32(uint32_t addr, uint32_t v);
// Block read of n words; handles 1 KB auto-increment boundaries.
int  dap_mem_read_block(uint32_t addr, uint32_t *buf, uint32_t nwords);

// Core debug helpers (require level >= ARMED for writes)
int  dap_core_halt(void);
int  dap_core_resume(void);
int  dap_core_read_reg(uint8_t regsel, uint32_t *v);   // only while halted
int  dap_core_regs(uint32_t regs[17]);                 // r0-r12, sp, lr, pc, xpsr
