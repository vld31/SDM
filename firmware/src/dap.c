// Universal Black Box - ADIv5/ADIv6 debug port + memory access port layer
#include "dap.h"
#include "swd.h"
#include "pico/time.h"

static dap_profile_t prof;
static int      cur_core = 0;
static uint32_t cur_select = 0xFFFFFFFFu;  // cached DP SELECT value
static bool     select_valid = false;
static bool     csw_valid = false;
static uint32_t csw_value = 0;

// AHB-AP CSW: DbgSwEnable | MasterType=debug | HPROT1 | AddrInc=single | Size=32-bit
#define CSW_WORD_INC 0xA2000012u

void dap_set_profile(const dap_profile_t *p) { prof = *p; }
const dap_profile_t *dap_profile(void) { return &prof; }

void dap_invalidate(void)
{
    select_valid = false;
    csw_valid = false;
}

static int map_err(int r)
{
    if (r == SWD_OK) return DAP_OK;
    if (r == SWD_ERR_PROTO) return DAP_ERR_NOTARGET;
    return DAP_ERR_SWD;
}

int dap_dp_read(uint8_t addr, uint32_t *v)
{
    return map_err(swd_transfer(false, true, addr, v));
}

int dap_dp_write(uint8_t addr, uint32_t v)
{
    return map_err(swd_transfer(false, false, addr, &v));
}

static int set_select(uint32_t sel)
{
    if (select_valid && cur_select == sel) return DAP_OK;
    int r = dap_dp_write(DP_SELECT, sel);
    if (r == DAP_OK) { cur_select = sel; select_valid = true; }
    else select_valid = false;
    return r;
}

// Compute the SELECT value that exposes AP register `reg` of the current core.
static inline uint32_t ap_select_value(uint32_t reg)
{
    if (prof.adiv6) return (prof.ap_base[cur_core] & 0xFFFFF000u) | (reg & 0xFF0u);
    return (prof.ap_base[cur_core] & 0xFF000000u) | (reg & 0xF0u);
}

static int ap_write(uint32_t reg, uint32_t v)
{
    int r = set_select(ap_select_value(reg));
    if (r) return r;
    return map_err(swd_transfer(true, false, (uint8_t)(reg & 0xC), &v));
}

// Posted AP read: issue the read, then collect the result from RDBUFF.
static int ap_read(uint32_t reg, uint32_t *v)
{
    int r = set_select(ap_select_value(reg));
    if (r) return r;
    uint32_t dummy;
    r = map_err(swd_transfer(true, true, (uint8_t)(reg & 0xC), &dummy));
    if (r) return r;
    return map_err(swd_transfer(false, true, DP_RDBUFF, v));
}

int dap_clear_errors(void)
{
    // ABORT: ORUNERRCLR | WDERRCLR | STKERRCLR | STKCMPCLR
    int r = dap_dp_write(DP_ABORT, 0x1E);
    csw_valid = false;
    return r;
}

static int ensure_csw(void)
{
    if (csw_valid && csw_value == CSW_WORD_INC) return DAP_OK;
    int r = ap_write(AP_CSW, CSW_WORD_INC);
    if (r == DAP_OK) { csw_valid = true; csw_value = CSW_WORD_INC; }
    return r;
}

int dap_mem_read32(uint32_t addr, uint32_t *v)
{
    int r = ensure_csw();
    if (r) return r;
    r = ap_write(AP_TAR, addr);
    if (r) return r;
    r = ap_read(AP_DRW, v);
    if (r) csw_valid = false;
    return r;
}

int dap_mem_write32(uint32_t addr, uint32_t v)
{
    int r = ensure_csw();
    if (r) return r;
    r = ap_write(AP_TAR, addr);
    if (r) return r;
    r = ap_write(AP_DRW, v);
    if (r) csw_valid = false;
    return r;
}

int dap_mem_read_block(uint32_t addr, uint32_t *buf, uint32_t nwords)
{
    int r = ensure_csw();
    if (r) return r;
    while (nwords) {
        // words until the next 1 KB boundary (TAR auto-increment guarantee)
        uint32_t to_boundary = (0x400u - (addr & 0x3FFu)) / 4u;
        uint32_t n = nwords < to_boundary ? nwords : to_boundary;
        r = ap_write(AP_TAR, addr);
        if (r) return r;
        r = set_select(ap_select_value(AP_DRW));
        if (r) return r;
        uint32_t v;
        // first DRW read is posted; each following read returns the previous word
        r = map_err(swd_transfer(true, true, AP_DRW & 0xC, &v));
        if (r) { csw_valid = false; return r; }
        for (uint32_t i = 1; i < n; i++) {
            r = map_err(swd_transfer(true, true, AP_DRW & 0xC, &v));
            if (r) { csw_valid = false; return r; }
            buf[i - 1] = v;
        }
        r = map_err(swd_transfer(false, true, DP_RDBUFF, &v));
        if (r) { csw_valid = false; return r; }
        buf[n - 1] = v;
        buf += n;
        addr += n * 4u;
        nwords -= n;
    }
    return DAP_OK;
}

static int power_up(void)
{
    int r = dap_dp_write(DP_CTRLSTAT, 0x50000000u);  // CDBGPWRUPREQ | CSYSPWRUPREQ
    if (r) return r;
    for (int i = 0; i < 1000; i++) {
        uint32_t st;
        r = dap_dp_read(DP_CTRLSTAT, &st);
        if (r) return r;
        if ((st & 0xA0000000u) == 0xA0000000u) return DAP_OK;  // CDBGPWRUPACK | CSYSPWRUPACK
        sleep_us(10);
    }
    return DAP_ERR_POWER;
}

static int select_dp(int core)
{
    dap_invalidate();
    if (!prof.adiv6 && prof.targetsel[core] != 0) {
        swd_line_reset();
        swd_write_targetsel(prof.targetsel[core]);
    }
    uint32_t id;
    int r = dap_dp_read(DP_DPIDR, &id);
    if (r) return r;
    cur_core = core;
    return DAP_OK;
}

int dap_connect(uint32_t *dpidr_out)
{
    dap_invalidate();
    cur_core = 0;
    // Cover every DP state: JTAG, dormant, or already SWD.
    swd_line_reset();
    swd_jtag_to_swd();
    swd_dormant_to_swd();
    swd_line_reset();
    if (!prof.adiv6 && prof.targetsel[0] != 0) {
        swd_write_targetsel(prof.targetsel[0]);
    }
    uint32_t id = 0;
    int r = dap_dp_read(DP_DPIDR, &id);
    if (r) return DAP_ERR_NOTARGET;
    if (dpidr_out) *dpidr_out = id;
    r = dap_clear_errors();
    if (r) return r;
    r = power_up();
    if (r) return r;
    csw_valid = false;
    r = ensure_csw();
    if (r) return r;
    // ADIv5 multidrop: power up the second DP as well so later switches are cheap.
    if (!prof.adiv6 && prof.targetsel[1] != 0) {
        r = select_dp(1);
        if (r == DAP_OK) {
            dap_clear_errors();
            power_up();
        }
        r = select_dp(0);
        if (r) return r;
        csw_valid = false;
        r = ensure_csw();
    }
    return r;
}

int dap_select_core(int core)
{
    if (core == cur_core) return DAP_OK;
    if (prof.adiv6) {
        cur_core = core;
        select_valid = false;
        csw_valid = false;   // each AP has its own CSW
        return DAP_OK;
    }
    if (prof.targetsel[core] == 0) return DAP_ERR_NOTARGET;
    int r = select_dp(core);
    if (r) return r;
    csw_valid = false;
    return DAP_OK;
}

int dap_current_core(void) { return cur_core; }

int dap_core_halt(void)
{
    int r = dap_mem_write32(SCS_DHCSR, DHCSR_KEY | DHCSR_C_DEBUGEN | DHCSR_C_HALT);
    if (r) return r;
    for (int i = 0; i < 200; i++) {
        uint32_t v;
        r = dap_mem_read32(SCS_DHCSR, &v);
        if (r) return r;
        if (v & DHCSR_S_HALT) return DAP_OK;
        sleep_us(5);
    }
    return DAP_ERR_TIMEOUT;
}

int dap_core_resume(void)
{
    return dap_mem_write32(SCS_DHCSR, DHCSR_KEY | DHCSR_C_DEBUGEN);
}

int dap_core_read_reg(uint8_t regsel, uint32_t *v)
{
    int r = dap_mem_write32(SCS_DCRSR, regsel);   // REGWnR = 0 (read)
    if (r) return r;
    for (int i = 0; i < 100; i++) {
        uint32_t d;
        r = dap_mem_read32(SCS_DHCSR, &d);
        if (r) return r;
        if (d & DHCSR_S_REGRDY) return dap_mem_read32(SCS_DCRDR, v);
    }
    return DAP_ERR_TIMEOUT;
}

int dap_core_regs(uint32_t regs[17])
{
    for (uint8_t i = 0; i < 17; i++) {
        // DCRSR: 0-12 = r0-r12, 13 = sp (current), 14 = lr, 15 = pc, 16 = xPSR
        int r = dap_core_read_reg(i, &regs[i]);
        if (r) return r;
    }
    return DAP_OK;
}
