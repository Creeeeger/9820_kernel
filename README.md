• The PLL_CON0 for G3D is written in the RA PLL programming path, not in the
  read site. The actual write happens in ra_pll_set_pmsk() (and enable/disable
  toggles happen in ra_enable_pll()), which are invoked during a VCLK rate
  transition.

  Here’s the concrete call path and where the register is updated:

  - Rate change entry: cal_dfs_set_rate() calls vclk_set_rate() for non‑ACPM
    VCLKs in cal-if/cal-if.c:90-107.
  - VCLK transition chooses a LUT and runs the transition in cal-if/vclk.c:181-
    189, which calls ra_set_clk_by_type(... PLL_TYPE ...).
  - ra_set_clk_by_type() calls ra_set_value() for PLLs in cal-if/ra.c:1678-1700
    → cal-if/ra.c:1246-1289.
  - ra_set_value() calls ra_set_pll() (cal-if/ra.c:604-744), which disables the
    PLL then programs it.
  - The actual PLL_CON0 write is in ra_pll_set_pmsk() (cal-if/ra.c:233-368),
    specifically writel(pll_con0, clk->pll_con0) at cal-if/ra.c:347-349.
    ra_enable_pll() also writes clk->pll_con0 when toggling mux/enable (cal-if/
    ra.c:203-223).

  How clk->pll_con0 points at the G3D PLL register:

  - During init, ra_init() calls ra_get_pll_address() for each PLL (cal-if/
    ra.c:2042-2067).
  - ra_get_pll_address() resolves clk->pll_con0 using the SFR access table (cal-
    if/ra.c:1424-1524).
  - The G3D PLL SFR is defined as SFR(PLL_CON0_PLL_G3D, 0x140, CMU_CMU) in cal-
    if/exynos9820/cmucal-sfr.c:63-64, and CMU_CMU base is 0x1a240000 (cal-if/
    exynos9820/cmucal-sfr.c:6-8). So the physical address is 0x1a240140, and
    clk->pll_con0 is the ioremapped VA for that.
  - The enable/stable/div field indices for PLL_G3D come from
    SFR_ACCESS(PLL_CON0_PLL_G3D_*) in cal-if/exynos9820/cmucal-sfr.c:2760-2765,
    and the PLL itself is wired in cmucal-node.c (PLL_G3D uses those indices at
    cal-if/exynos9820/cmucal-node.c:153).

  Additional (non‑DVFS) writes:

  - Power sequencing also writes PLL_CON0_PLL_G3D via PMUCAL sequences (cal-if/
    exynos9820/flexpmu_cal_local_exynos9820.h:556-593), but that’s for power on/
    off rather than the VCLK DVFS path.


---

PLL_G3D never gets programmed for that VCLK
      - If G3D DVFS is going through ACPM (IS_ACPM_VCLK(id)), the kernel doesn’t touch PLL_CON0 at all (cal-if/cal-if.c:90-107).
      - Or the DVFS clock list for dvfs_g3d does not include PLL_G3D, so ra_set_pll() isn’t called for it.