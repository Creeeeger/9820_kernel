#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/percpu.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/types.h>
#include <soc/samsung/ect_parser.h>
#include <soc/samsung/exynos-pmu.h>

#include "cmucal.h"
#include "ra.h"

static DEFINE_PER_CPU(bool, ra_log_enabled);

struct ra_log_ctx {
    bool prev_enabled;
};

static bool ra_is_g3d_name(const char *name) {
    if (!name)
        return false;

    return strstr(name, "g3d") || strstr(name, "G3D");
}

static bool ra_should_log(unsigned int id, const char *name) {
    return id == 10 || GET_IDX(id) == 10 || ra_is_g3d_name(name);
}

static struct ra_log_ctx ra_log_push(unsigned int id, const char *name) {
    struct ra_log_ctx ctx;

    migrate_disable();
    ctx.prev_enabled = this_cpu_read(ra_log_enabled);
    this_cpu_write(ra_log_enabled, ra_should_log(id, name));

    return ctx;
}

static void ra_log_pop(struct ra_log_ctx ctx) {
    this_cpu_write(ra_log_enabled, ctx.prev_enabled);
    migrate_enable();
}

static bool ra_log_current(void) {
    return this_cpu_read(ra_log_enabled);
}

static inline struct ra_log_ctx ra_log_push_clk(struct cmucal_clk *clk,
                                                unsigned int id) {
    return ra_log_push(id, clk ? clk->name : NULL);
}

static bool ra_list_has_g3d(unsigned int *list, unsigned int num_list) {
    unsigned int i;

    if (!list)
        return false;

    for (i = 0; i < num_list; i++) {
        struct cmucal_clk *clk;

        if (ra_should_log(list[i], NULL))
            return true;

        clk = cmucal_get_node(list[i]);
        if (ra_should_log(list[i], clk ? clk->name : NULL))
            return true;
    }

    return false;
}

#undef pr_info
#define pr_info(fmt, ...)                                                    \
    do {                                                                     \
        if (ra_log_current())                                                \
            printk(KERN_INFO pr_fmt(fmt), ##__VA_ARGS__);                    \
    } while (0)

static enum trans_opt ra_get_trans_opt(unsigned int to, unsigned int from) {
    if (from == to)
        return TRANS_IGNORE;

    return to > from ? TRANS_HIGH : TRANS_LOW;
}

static int ra_wait_done(void __iomem *reg, unsigned char shift,
                        unsigned int done, int usec) {
    unsigned int result;

    do {
        result = get_bit(reg, shift);

        if (result == done)
            return 0;
        udelay(1);
    } while (--usec > 0);

    return -EVCLKTIMEOUT;
}

static unsigned int ra_get_fixed_rate(struct cmucal_clk *clk) {
    struct cmucal_clk_fixed_rate *frate = to_fixed_rate_clk(clk);
    void __iomem *offset;
    unsigned int rate;
    u32 val;

    pr_info("CMUCAL: ra_get_fixed_rate: enter clk=%p enable=%p frate=%p\n", clk,
            clk ? clk->enable : NULL, frate);

    if (!clk) {
        pr_info("CMUCAL: ra_get_fixed_rate: ERROR clk is NULL -> return 0\n");
        return 0;
    }

    /* frate is derived from clk, so log after NULL check */
    pr_info("CMUCAL: ra_get_fixed_rate: fixed_rate=%u FIN_HZ_26M=%u "
            "PLL_MUX_SEL=0x%x\n",
            frate->fixed_rate, (unsigned int)FIN_HZ_26M,
            (unsigned int)PLL_MUX_SEL);

    if (!clk->enable) {
        pr_info("CMUCAL: ra_get_fixed_rate: clk->enable is NULL -> return "
                "fixed_rate=%u\n",
                frate->fixed_rate);
        return frate->fixed_rate;
    }

    offset = convert_pll_base(clk->enable);
    pr_info(
        "CMUCAL: ra_get_fixed_rate: convert_pll_base(enable=%p) -> offset=%p\n",
        clk->enable, offset);

    /* Be loud if convert_pll_base returns NULL; readl(NULL) will explode */
    if (!offset) {
        pr_info("CMUCAL: ra_get_fixed_rate: ERROR offset is NULL -> return "
                "fixed_rate=%u\n",
                frate->fixed_rate);
        return frate->fixed_rate;
    }

    val = readl(offset);
    pr_info("CMUCAL: ra_get_fixed_rate: readl(offset=%p)=0x%08x (PLL_MUX_SEL "
            "set? %d)\n",
            offset, val, !!(val & PLL_MUX_SEL));

    if (val & PLL_MUX_SEL) {
        rate = frate->fixed_rate;
        pr_info("CMUCAL: ra_get_fixed_rate: mux=PLL -> rate=fixed_rate=%u\n",
                rate);
    } else {
        rate = FIN_HZ_26M;
        pr_info("CMUCAL: ra_get_fixed_rate: mux=FIN -> rate=FIN_HZ_26M=%u\n",
                rate);
    }

    pr_info("CMUCAL: ra_get_fixed_rate: exit rate=%u\n", rate);
    return rate;
}

static unsigned int ra_get_fixed_factor(struct cmucal_clk *clk) {
    struct cmucal_clk_fixed_factor *ffacor = to_fixed_factor_clk(clk);

    return ffacor->ratio;
}

static struct cmucal_pll_table *get_pll_table(struct cmucal_pll *pll_clk,
                                              unsigned long rate,
                                              unsigned long rate_hz) {
    struct cmucal_pll_table *prate_table;
    int i;
    unsigned long matching;

    pr_info(
        "CMUCAL: get_pll_table: enter pll_clk=%p rate(kHz?)=%lu rate_hz=%lu\n",
        pll_clk, rate, rate_hz);

    if (!pll_clk) {
        pr_info(
            "CMUCAL: get_pll_table: ERROR pll_clk is NULL -> return NULL\n");
        return NULL;
    }

    prate_table = pll_clk->rate_table;

    pr_info("CMUCAL: get_pll_table: rate_table=%p rate_count=%d\n", prate_table,
            pll_clk->rate_count);

    if (!prate_table || pll_clk->rate_count <= 0) {
        pr_info("CMUCAL: get_pll_table: ERROR empty rate table (table=%p "
                "count=%d) -> NULL\n",
                prate_table, pll_clk->rate_count);
        return NULL;
    }

    /* Optional: dump first few entries to sanity check table content */
    for (i = 0; i < pll_clk->rate_count && i < 8; i++)
        pr_info("CMUCAL: get_pll_table: table[%d]=%p rate=%lu\n", i,
                &prate_table[i], prate_table[i].rate);

    if (rate_hz) {
        matching = rate_hz;
        pr_info("CMUCAL: get_pll_table: rate_hz matching enabled, start "
                "matching=%lu\n",
                matching);

        /* Skip pure Hz unit matching (comment in original). */

        /* 10Hz unit */
        do_div(matching, 10);
        pr_info("CMUCAL: get_pll_table: try 10Hz unit: matching=%lu (compare "
                "to table.rate/10)\n",
                matching);

        for (i = 0; i < pll_clk->rate_count; i++) {
            unsigned long table_scaled = prate_table[i].rate / 10;
            if (matching == table_scaled) {
                pr_info("CMUCAL: get_pll_table: HIT 10Hz: i=%d entry=%p "
                        "table.rate=%lu table.rate/10=%lu\n",
                        i, &prate_table[i], prate_table[i].rate, table_scaled);
                return &prate_table[i];
            }
        }

        /* Fallback: 100Hz unit */
        do_div(matching, 10);
        pr_info("CMUCAL: get_pll_table: try 100Hz unit: matching=%lu (compare "
                "to table.rate/100)\n",
                matching);

        for (i = 0; i < pll_clk->rate_count; i++) {
            unsigned long table_scaled = prate_table[i].rate / 100;
            if (matching == table_scaled) {
                pr_info("CMUCAL: get_pll_table: HIT 100Hz: i=%d entry=%p "
                        "table.rate=%lu table.rate/100=%lu\n",
                        i, &prate_table[i], prate_table[i].rate, table_scaled);
                return &prate_table[i];
            }
        }

        /* Fallback: 1000Hz unit -> handled below by rate matching */
        pr_info("CMUCAL: get_pll_table: no hit in 10Hz/100Hz fallbacks, will "
                "try 1000Hz(rate) below\n");
    }

    /* 1000Hz unit (kHz) */
    pr_info("CMUCAL: get_pll_table: try 1000Hz unit: target rate=%lu (compare "
            "to table.rate/1000)\n",
            rate);

    for (i = 0; i < pll_clk->rate_count; i++) {
        unsigned long table_scaled = prate_table[i].rate / 1000;
        if (rate == table_scaled) {
            pr_info("CMUCAL: get_pll_table: HIT 1000Hz: i=%d entry=%p "
                    "table.rate=%lu table.rate/1000=%lu\n",
                    i, &prate_table[i], prate_table[i].rate, table_scaled);
            return &prate_table[i];
        }
    }

    pr_info("CMUCAL: get_pll_table: MISS: no matching entry for rate=%lu "
            "rate_hz=%lu\n",
            rate, rate_hz);
    return NULL;
}

static int ra_is_pll_enabled(struct cmucal_clk *clk) {
    return get_bit(clk->pll_con0, clk->e_shift);
}

static int ra_enable_pll(struct cmucal_clk *clk, int enable) {
    unsigned int reg;
    int ret = 0;

    reg = readl(clk->pll_con0);
    if (!enable) {
        reg &= ~(PLL_MUX_SEL);
        writel(reg, clk->pll_con0);

        ret = ra_wait_done(clk->pll_con0, PLL_MUX_BUSY_SHIFT, 0, 100);
        if (ret)
            pr_err("pll mux change time out, \'%s\'\n", clk->name);
    }

    if (enable)
        reg |= 1 << clk->e_shift;
    else
        reg &= ~(1 << clk->e_shift);

    writel(reg, clk->pll_con0);

    if (enable) {
        ret = ra_wait_done(clk->pll_con0, clk->s_shift, 1, 100);
        if (ret)
            pr_err("pll time out, \'%s\' %d\n", clk->name, enable);
    }

    return ret;
}

static int ra_pll_set_pmsk(struct cmucal_clk *clk,
                           struct cmucal_pll_table *rate_table) {
    struct cmucal_pll *pll;
    unsigned int mdiv, pdiv, sdiv, pll_con0, pll_con1 = 0;
    signed short kdiv = 0;
    int ret = 0;

    pr_info("CMUCAL: ra_pll_set_pmsk: enter clk=%p rate_table=%p\n", clk,
            rate_table);

    if (!clk || !rate_table) {
        pr_info(
            "CMUCAL: ra_pll_set_pmsk: ERROR clk(%p) or rate_table(%p) NULL\n",
            clk, rate_table);
        return -EINVAL;
    }

    pll = to_pll_clk(clk);

    pr_info("CMUCAL: ra_pll_set_pmsk: clk.name=%s pll=%p pll_con0=%p "
            "pll_con1=%p lock=%p e_shift=%u s_shift=%u\n",
            clk->name ? clk->name : "(null)", pll, clk->pll_con0, clk->pll_con1,
            clk->lock, clk->e_shift, clk->s_shift);

    pr_info("CMUCAL: ra_pll_set_pmsk: pll fields: m_width=%u m_shift=%u "
            "p_width=%u p_shift=%u s_width=%u s_shift=%u k_width=%u k_shift=%u "
            "lock_time=%u flock_time=%u frac=%d\n",
            pll->m_width, pll->m_shift, pll->p_width, pll->p_shift,
            pll->s_width, pll->s_shift, pll->k_width, pll->k_shift,
            pll->lock_time, pll->flock_time, is_frac_pll(pll));

    pdiv = rate_table->pdiv;
    mdiv = rate_table->mdiv;
    sdiv = rate_table->sdiv;
    kdiv = rate_table->kdiv;

    pr_info("CMUCAL: ra_pll_set_pmsk: table pdiv=%u mdiv=%u sdiv=%u kdiv=%d "
            "table.rate=%lu (if present)\n",
            pdiv, mdiv, sdiv, kdiv, rate_table->rate);

    if (!clk->pll_con0) {
        pr_info("CMUCAL: ra_pll_set_pmsk: ERROR clk->pll_con0 is NULL -> "
                "-EVCLKNOENT\n");
        return -EVCLKNOENT;
    }

    pll_con0 = readl(clk->pll_con0);
    pr_info("CMUCAL: ra_pll_set_pmsk: read pll_con0[%p]=0x%08x\n",
            clk->pll_con0, pll_con0);

    {
        unsigned int m_mask = get_mask(pll->m_width, pll->m_shift);
        unsigned int p_mask = get_mask(pll->p_width, pll->p_shift);
        unsigned int s_mask = get_mask(pll->s_width, pll->s_shift);

        pr_info("CMUCAL: ra_pll_set_pmsk: masks m=0x%08x p=0x%08x s=0x%08x\n",
                m_mask, p_mask, s_mask);

        pll_con0 &= ~(m_mask | p_mask | s_mask);
        pr_info("CMUCAL: ra_pll_set_pmsk: pll_con0 after clear=0x%08x\n",
                pll_con0);

        pll_con0 |= ((mdiv << pll->m_shift) | (pdiv << pll->p_shift) |
                     (sdiv << pll->s_shift));

        pr_info("CMUCAL: ra_pll_set_pmsk: pll_con0 after set PMS=0x%08x (m<<%u "
                "p<<%u s<<%u)\n",
                pll_con0, pll->m_shift, pll->p_shift, pll->s_shift);
    }

    pll_con0 |= PLL_MUX_SEL | (1U << clk->e_shift);
    pr_info("CMUCAL: ra_pll_set_pmsk: pll_con0 after mux+enable=0x%08x "
            "(PLL_MUX_SEL=0x%x e_shift=%u)\n",
            pll_con0, (unsigned int)PLL_MUX_SEL, clk->e_shift);

    if (is_frac_pll(pll)) {
        unsigned int lock_val;

        lock_val = pdiv * (kdiv ? pll->flock_time : pll->lock_time);
        pr_info("CMUCAL: ra_pll_set_pmsk: frac pll: writing lock=%u to "
                "lock[%p] (pdiv=%u kdiv=%d)\n",
                lock_val, clk->lock, pdiv, kdiv);
        writel(lock_val, clk->lock);

        if (clk->pll_con1) {
            unsigned int k_mask = get_mask(pll->k_width, pll->k_shift);

            pll_con1 = readl(clk->pll_con1);
            pr_info("CMUCAL: ra_pll_set_pmsk: read pll_con1[%p]=0x%08x\n",
                    clk->pll_con1, pll_con1);

            pr_info("CMUCAL: ra_pll_set_pmsk: k_mask=0x%08x (k_width=%u "
                    "k_shift=%u)\n",
                    k_mask, pll->k_width, pll->k_shift);

            pll_con1 &= ~k_mask;
            pll_con1 |= ((unsigned int)(kdiv) << pll->k_shift);

            pr_info("CMUCAL: ra_pll_set_pmsk: write pll_con1[%p]=0x%08x "
                    "(kdiv<<%u)\n",
                    clk->pll_con1, pll_con1, pll->k_shift);
            writel(pll_con1, clk->pll_con1);
        } else {
            pr_info("CMUCAL: ra_pll_set_pmsk: frac pll: clk->pll_con1 is NULL, "
                    "skipping KD write\n");
        }
    } else {
        unsigned int lock_val = pdiv * pll->lock_time;
        pr_info(
            "CMUCAL: ra_pll_set_pmsk: int pll: writing lock=%u to lock[%p]\n",
            lock_val, clk->lock);
        writel(lock_val, clk->lock);
    }

    pr_info("CMUCAL: ra_pll_set_pmsk: write pll_con0[%p]=0x%08x\n",
            clk->pll_con0, pll_con0);
    writel(pll_con0, clk->pll_con0);

    ret = ra_wait_done(clk->pll_con0, clk->s_shift, 1, 100);
    pr_info("CMUCAL: ra_pll_set_pmsk: ra_wait_done(con0=%p s_shift=%u "
            "target=%u timeout=%u) -> ret=%d\n",
            clk->pll_con0, clk->s_shift, 1U, 100U, ret);

    if (ret)
        pr_err("time out, '%s'\n", clk->name);

    pr_info("CMUCAL: ra_pll_set_pmsk: exit ret=%d final pll_con0=0x%08x\n", ret,
            readl(clk->pll_con0));
    if (clk->pll_con1)
        pr_info("CMUCAL: ra_pll_set_pmsk: final pll_con1[%p]=0x%08x\n",
                clk->pll_con1, readl(clk->pll_con1));
    if (clk->lock)
        pr_info("CMUCAL: ra_pll_set_pmsk: final lock[%p]=0x%08x\n", clk->lock,
                readl(clk->lock));

    return ret;
}

static int ra_get_div_mux(struct cmucal_clk *clk) {
    int val;

    pr_info("CMUCAL: ra_get_div_mux: enter clk=%p name=%s offset=%p shift=%u "
            "width=%u\n",
            clk, (clk && clk->name) ? clk->name : "(null)",
            clk ? clk->offset : NULL, clk ? clk->shift : 0,
            clk ? clk->width : 0);

    if (!clk) {
        pr_info("CMUCAL: ra_get_div_mux: ERROR clk is NULL -> return 0\n");
        return 0;
    }

    if (!clk->offset) {
        pr_info("CMUCAL: ra_get_div_mux: clk->offset is NULL -> return 0\n");
        return 0;
    }

    val = get_value(clk->offset, clk->shift, clk->width);
    pr_info("CMUCAL: ra_get_div_mux: get_value(offset=%p shift=%u width=%u) -> "
            "%d (reg=0x%08x)\n",
            clk->offset, clk->shift, clk->width, val, readl(clk->offset));

    return val;
}

static int ra_set_div_mux(struct cmucal_clk *clk, unsigned int params) {
    unsigned int reg;
    int ret = 0;

    pr_info("CMUCAL: ra_set_div_mux: enter clk=%p name=%s params=%u offset=%p "
            "shift=%u width=%u status=%p s_shift=%u\n",
            clk, (clk && clk->name) ? clk->name : "(null)", params,
            clk ? clk->offset : NULL, clk ? clk->shift : 0,
            clk ? clk->width : 0, clk ? clk->status : NULL,
            clk ? clk->s_shift : 0);

    if (!clk) {
        pr_info(
            "CMUCAL: ra_set_div_mux: ERROR clk is NULL -> return -EINVAL\n");
        return -EINVAL;
    }

    if (!clk->offset) {
        pr_info("CMUCAL: ra_set_div_mux: clk->offset is NULL -> return 0\n");
        return 0;
    }

    pr_info("CMUCAL: ra_set_div_mux: before write: offset[%p]=0x%08x\n",
            clk->offset, readl(clk->offset));

    reg = clear_value(clk->offset, clk->width, clk->shift);
    pr_info("CMUCAL: ra_set_div_mux: clear_value(offset=%p width=%u shift=%u) "
            "-> reg=0x%08x\n",
            clk->offset, clk->width, clk->shift, reg);

    pr_info("CMUCAL: ra_set_div_mux: writel(offset=%p, val=0x%08x) "
            "(params<<shift=0x%08x)\n",
            clk->offset, reg | (params << clk->shift), (params << clk->shift));
    writel(reg | (params << clk->shift), clk->offset);

    pr_info("CMUCAL: ra_set_div_mux: after write: offset[%p]=0x%08x\n",
            clk->offset, readl(clk->offset));

    if (clk->status == NULL) {
        pr_info("CMUCAL: ra_set_div_mux: status is NULL -> skip wait_done, "
                "return 0\n");
        return 0;
    }

    pr_info("CMUCAL: ra_set_div_mux: wait_done: status[%p] s_shift=%u "
            "target=%u timeout=%u\n",
            clk->status, clk->s_shift, 0U, 100U);

    ret = ra_wait_done(clk->status, clk->s_shift, 0, 100);
    pr_info(
        "CMUCAL: ra_set_div_mux: ra_wait_done -> ret=%d status[%p]=0x%08x\n",
        ret, clk->status, readl(clk->status));

    if (ret) {
        pr_err("time out, '%s' [%p]=%x [%p]=%x\n", clk->name, clk->offset,
               readl(clk->offset), clk->status, readl(clk->status));
    }

    return ret;
}

static int ra_set_mux_rate(struct cmucal_clk * clk, unsigned int rate) {
        struct cmucal_mux *mux;
        unsigned int p_rate, sel = 0;
        unsigned int diff, min_diff = 0xFFFFFFFF;
        int i;
        int ret = -EVCLKINVAL;

        pr_info("CMUCAL: ra_set_mux_rate: enter clk=%p name=%s rate=%u\n", clk,
                (clk && clk->name) ? clk->name : "(null)", rate);

        if (!clk) {
            pr_info("CMUCAL: ra_set_mux_rate: ERROR clk is NULL -> -EINVAL\n");
            return -EINVAL;
        }

        if (rate == 0) {
            pr_info("CMUCAL: ra_set_mux_rate: rate==0 -> ret=%d\n", ret);
            return ret;
        }

        mux = to_mux_clk(clk);
        pr_info("CMUCAL: ra_set_mux_rate: mux=%p num_parents=%u pid[]=%p\n",
                mux, mux->num_parents, mux->pid);

        if (!mux->num_parents) {
            pr_info("CMUCAL: ra_set_mux_rate: ERROR num_parents==0 -> "
                    "-EVCLKINVAL\n");
            return -EVCLKINVAL;
        }

        for (i = 0; i < mux->num_parents; i++) {
            p_rate = ra_recalc_rate(mux->pid[i]);

            pr_info("CMUCAL: ra_set_mux_rate: parent[%d] pid=%u -> p_rate=%u "
                    "(target=%u)\n",
                    i, mux->pid[i], p_rate, rate);

            if (p_rate == rate) {
                sel = i;
                pr_info("CMUCAL: ra_set_mux_rate: exact match: sel=%u\n", sel);
                break;
            }

            diff = abs((int)p_rate - (int)rate);
            if (diff < min_diff) {
                min_diff = diff;
                sel = i;
                pr_info("CMUCAL: ra_set_mux_rate: new best approx: sel=%u "
                        "min_diff=%u (p_rate=%u)\n",
                        sel, min_diff, p_rate);
            }
        }

        if (i == mux->num_parents)
            pr_info("CMUCAL: ra_set_mux_rate: approx select %s target=%u "
                    "min_diff=%u sel=%u\n",
                    clk->name, rate, min_diff, sel);

        pr_info("CMUCAL: ra_set_mux_rate: ra_set_div_mux(clk=%p, sel=%u)\n",
                clk, sel);
        ret = ra_set_div_mux(clk, sel);
        pr_info("CMUCAL: ra_set_mux_rate: exit ret=%d\n", ret);

        return ret;
    }

    static int ra_set_div_rate(struct cmucal_clk * clk, unsigned int rate) {
        unsigned int p_rate;
        unsigned int ratio, max_ratio;
        unsigned int diff1 = 0, diff2 = 0;
        int ret = -EVCLKINVAL;

        pr_info("CMUCAL: ra_set_div_rate: enter clk=%p name=%s rate=%u pid=%u "
                "width=%u shift=%u offset=%p status=%p s_shift=%u\n",
                clk, (clk && clk->name) ? clk->name : "(null)", rate,
                clk ? clk->pid : 0, clk ? clk->width : 0, clk ? clk->shift : 0,
                clk ? clk->offset : NULL, clk ? clk->status : NULL,
                clk ? clk->s_shift : 0);

        if (!clk) {
            pr_info("CMUCAL: ra_set_div_rate: ERROR clk is NULL -> -EINVAL\n");
            return -EINVAL;
        }

        if (rate == 0) {
            pr_info("CMUCAL: ra_set_div_rate: rate==0 -> ret=%d\n", ret);
            return ret;
        }

        p_rate = ra_recalc_rate(clk->pid);
        pr_info("CMUCAL: ra_set_div_rate: parent pid=%u -> p_rate=%u\n",
                clk->pid, p_rate);

        if (p_rate == 0) {
            pr_info("CMUCAL: ra_set_div_rate: p_rate==0 -> ret=%d\n", ret);
            return ret;
        }

        max_ratio = width_to_mask(clk->width) + 1;
        ratio = p_rate / rate;

        pr_info("CMUCAL: ra_set_div_rate: compute ratio=p_rate/rate=%u/%u=%u "
                "max_ratio=%u (width_to_mask+1)\n",
                p_rate, rate, ratio, max_ratio);

        if (ratio > 0 && ratio <= max_ratio) {
            if (p_rate % rate) {
                diff1 = p_rate - (ratio * rate);
                diff2 = (ratio * rate) + rate - p_rate;

                pr_info("CMUCAL: ra_set_div_rate: non-integer division: "
                        "p_rate%%rate=%u diff1=%u diff2=%u\n",
                        p_rate % rate, diff1, diff2);

                if (diff1 > diff2) {
                    pr_info("CMUCAL: ra_set_div_rate: rounding up: "
                            "ra_set_div_mux(clk=%p, params=%u)\n",
                            clk, ratio);
                    ret = ra_set_div_mux(clk, ratio);
                    pr_info("CMUCAL: ra_set_div_rate: exit ret=%d\n", ret);
                    return ret;
                }
            }

            pr_info("CMUCAL: ra_set_div_rate: rounding down/default: "
                    "ra_set_div_mux(clk=%p, params=%u)\n",
                    clk, ratio - 1);
            ret = ra_set_div_mux(clk, ratio - 1);
        } else if (ratio == 0) {
            pr_info("CMUCAL: ra_set_div_rate: ratio==0: ra_set_div_mux(clk=%p, "
                    "params=%u)\n",
                    clk, ratio);
            ret = ra_set_div_mux(clk, ratio);
        } else {
            pr_err("failed div_rate %s %u:%u:%u:%u\n", clk->name, p_rate, rate,
                   ratio, max_ratio);
            pr_info("CMUCAL: ra_set_div_rate: ERROR branch: ratio=%u "
                    "max_ratio=%u\n",
                    ratio, max_ratio);
        }

        pr_info("CMUCAL: ra_set_div_rate: exit ret=%d\n", ret);
        return ret;
    }

    static int ra_set_pll(struct cmucal_clk * clk, unsigned int rate,
                          unsigned int rate_hz) {
        struct cmucal_pll *pll;
        struct cmucal_pll_table *rate_table;
        struct cmucal_pll_table table;
        struct cmucal_clk *umux;
        unsigned int fin;
        int ret = 0;

        pr_info("CMUCAL: ra_set_pll: enter clk=%p name=%s rate(kHz?)=%u "
                "rate_hz=%u pid=%u\n",
                clk, (clk && clk->name) ? clk->name : "(null)", rate, rate_hz,
                clk ? clk->pid : 0);

        if (!clk) {
            pr_info("CMUCAL: ra_set_pll: ERROR clk is NULL -> -EINVAL\n");
            return -EINVAL;
        }

        pll = to_pll_clk(clk);

        pr_info("CMUCAL: ra_set_pll: pll=%p umux=%u (EMPTY_CLK_ID=%u)\n", pll,
                pll->umux, (unsigned int)EMPTY_CLK_ID);

        if (rate == 0) {
            pr_info("CMUCAL: ra_set_pll: rate==0 path: will switch umux->0 (if "
                    "exists) and disable pll\n");

            if (pll->umux != EMPTY_CLK_ID) {
                umux = cmucal_get_node(pll->umux);
                pr_info("CMUCAL: ra_set_pll: cmucal_get_node(umux=%u) -> %p\n",
                        pll->umux, umux);

                if (umux) {
                    pr_info("CMUCAL: ra_set_pll: ra_set_div_mux(umux=%p "
                            "name=%s, 0)\n",
                            umux, umux->name ? umux->name : "(null)");
                    ra_set_div_mux(umux, 0);
                } else {
                    pr_info("CMUCAL: ra_set_pll: WARNING umux node is NULL\n");
                }
            } else {
                pr_info("CMUCAL: ra_set_pll: no umux (EMPTY_CLK_ID)\n");
            }

            pr_info("CMUCAL: ra_set_pll: ra_enable_pll(clk=%p name=%s, 0)\n",
                    clk, clk->name ? clk->name : "(null)");
            ra_enable_pll(clk, 0);

            pr_info("CMUCAL: ra_set_pll: exit rate==0 ret=%d\n", 0);
            return 0;
        }

        pr_info("CMUCAL: ra_set_pll: rate!=0 path: get_pll_table(pll=%p, "
                "rate=%u, rate_hz=%u)\n",
                pll, rate, rate_hz);
        rate_table = get_pll_table(pll, rate, rate_hz);
        pr_info("CMUCAL: ra_set_pll: get_pll_table -> rate_table=%p\n",
                rate_table);

        if (!rate_table) {
            pr_info("CMUCAL: ra_set_pll: no matching table entry; computing "
                    "fin and running pll_find_table\n");

            if (IS_FIXED_RATE(clk->pid)) {
                fin = ra_get_value(clk->pid);
                pr_info("CMUCAL: ra_set_pll: IS_FIXED_RATE(pid=%u)=1 -> "
                        "fin=ra_get_value=%u\n",
                        clk->pid, fin);
            } else {
                fin = FIN_HZ_26M;
                pr_info("CMUCAL: ra_set_pll: IS_FIXED_RATE(pid=%u)=0 -> "
                        "fin=FIN_HZ_26M=%u\n",
                        clk->pid, fin);
            }

            ret = pll_find_table(pll, &table, fin, rate, rate_hz);
            pr_info("CMUCAL: ra_set_pll: pll_find_table(pll=%p, fin=%u, "
                    "rate=%u, rate_hz=%u) -> ret=%d\n",
                    pll, fin, rate, rate_hz, ret);

            if (ret) {
                pr_err("failed %s table %u\n", clk->name, rate);
                pr_info("CMUCAL: ra_set_pll: ERROR pll_find_table failed -> "
                        "ret=%d\n",
                        ret);
                return ret;
            }

            /* dump the synthesized table entry */
            pr_info("CMUCAL: ra_set_pll: synthesized table: rate=%lu pdiv=%u "
                    "mdiv=%u sdiv=%u kdiv=%d\n",
                    table.rate, table.pdiv, table.mdiv, table.sdiv, table.kdiv);

            rate_table = &table;
            pr_info("CMUCAL: ra_set_pll: using synthesized rate_table=%p\n",
                    rate_table);
        } else {
            pr_info("CMUCAL: ra_set_pll: using matched rate_table=%p: rate=%lu "
                    "pdiv=%u mdiv=%u sdiv=%u kdiv=%d\n",
                    rate_table, rate_table->rate, rate_table->pdiv,
                    rate_table->mdiv, rate_table->sdiv, rate_table->kdiv);
        }

        /* Always disable before reprogramming */
        pr_info("CMUCAL: ra_set_pll: ra_enable_pll(clk=%p name=%s, 0) before "
                "programming\n",
                clk, clk->name ? clk->name : "(null)");
        ra_enable_pll(clk, 0);

        pr_info("CMUCAL: ra_set_pll: ra_pll_set_pmsk(clk=%p name=%s, "
                "rate_table=%p)\n",
                clk, clk->name ? clk->name : "(null)", rate_table);
        ret = ra_pll_set_pmsk(clk, rate_table);
        pr_info("CMUCAL: ra_set_pll: ra_pll_set_pmsk -> ret=%d\n", ret);

        if (ret) {
            pr_info("CMUCAL: ra_set_pll: ERROR: programming failed; skipping "
                    "umux switch\n");
            return ret;
        }

        if (pll->umux != EMPTY_CLK_ID) {
            umux = cmucal_get_node(pll->umux);
            pr_info("CMUCAL: ra_set_pll: cmucal_get_node(umux=%u) -> %p\n",
                    pll->umux, umux);

            if (umux) {
                pr_info(
                    "CMUCAL: ra_set_pll: ra_set_div_mux(umux=%p name=%s, 1)\n",
                    umux, umux->name ? umux->name : "(null)");
                ra_set_div_mux(umux, 1);
            } else {
                pr_info("CMUCAL: ra_set_pll: WARNING umux node is NULL\n");
            }
        } else {
            pr_info("CMUCAL: ra_set_pll: no umux (EMPTY_CLK_ID)\n");
        }

        pr_info("CMUCAL: ra_set_pll: exit ret=%d\n", ret);
        return ret;
    }

    static unsigned int ra_get_pll(struct cmucal_clk * clk) {
        struct cmucal_pll *pll;
        unsigned int mdiv, pdiv, sdiv, pll_con0;
        short kdiv = 0;
        unsigned long long fout;

        if (!clk) {
            pr_info("CMUCAL: ra_get_pll: ERROR clk is NULL -> return 0\n");
            return 0;
        }

        pr_info(
            "CMUCAL: ra_get_pll: enter clk=%p pid=%u pll_con0=%p pll_con1=%p\n",
            clk, clk->pid, clk->pll_con0, clk->pll_con1);

        if (!ra_is_pll_enabled(clk)) {
            pll_con0 = readl(clk->pll_con0);
            pr_info("CMUCAL: ra_get_pll: pll disabled: "
                    "readl(pll_con0=%p)=0x%08x PLL_MUX_SEL=%d\n",
                    clk->pll_con0, pll_con0, !!(pll_con0 & PLL_MUX_SEL));

            if (pll_con0 & PLL_MUX_SEL) {
                pr_info(
                    "CMUCAL: ra_get_pll: pll disabled + mux=PLL -> return 0\n");
                return 0;
            } else {
                pr_info("CMUCAL: ra_get_pll: pll disabled + mux=FIN -> return "
                        "FIN_HZ_26M=%u\n",
                        (unsigned int)FIN_HZ_26M);
                return FIN_HZ_26M;
            }
        }

        pll = to_pll_clk(clk);
        pr_info("CMUCAL: ra_get_pll: pll=%p rate_table=%p rate_count=%d "
                "shifts(m/p/s/k)=%u/%u/%u/%u widths(m/p/s/k)=%u/%u/%u/%u\n",
                pll, pll->rate_table, pll->rate_count, pll->m_shift,
                pll->p_shift, pll->s_shift, pll->k_shift, pll->m_width,
                pll->p_width, pll->s_width, pll->k_width);

        pll_con0 = readl(clk->pll_con0);
        mdiv = (pll_con0 >> pll->m_shift) & width_to_mask(pll->m_width);
        pdiv = (pll_con0 >> pll->p_shift) & width_to_mask(pll->p_width);
        sdiv = (pll_con0 >> pll->s_shift) & width_to_mask(pll->s_width);

        pr_info("CMUCAL: ra_get_pll: pll_con0=0x%08x -> mdiv=%u pdiv=%u "
                "sdiv=%u (m_mask=0x%x p_mask=0x%x s_mask=0x%x)\n",
                pll_con0, mdiv, pdiv, sdiv, width_to_mask(pll->m_width),
                width_to_mask(pll->p_width), width_to_mask(pll->s_width));

        if (IS_FIXED_RATE(clk->pid)) {
            fout = ra_get_value(clk->pid);
            pr_info("CMUCAL: ra_get_pll: input is FIXED rate: "
                    "ra_get_value(pid=%u) -> fin=%llu\n",
                    clk->pid, fout);
        } else {
            fout = FIN_HZ_26M;
            pr_info("CMUCAL: ra_get_pll: input is FIN: FIN_HZ_26M=%u\n",
                    (unsigned int)FIN_HZ_26M);
        }

        if (is_normal_pll(pll)) {
            unsigned long long before = fout;

            pr_info("CMUCAL: ra_get_pll: type=NORMAL: fout=%llu * mdiv=%u / "
                    "(pdiv=%u << sdiv=%u)\n",
                    fout, mdiv, pdiv, sdiv);

            fout *= mdiv;
            do_div(fout, (pdiv << sdiv));

            pr_info("CMUCAL: ra_get_pll: type=NORMAL: fin=%llu -> fout=%llu\n",
                    before, fout);
        } else if (is_frac_pll(pll) && clk->pll_con1) {
            unsigned long long before = fout;
            unsigned long long num;

            kdiv = get_value(clk->pll_con1, pll->k_shift, pll->k_width);
            pr_info("CMUCAL: ra_get_pll: type=FRAC: read kdiv from pll_con1=%p "
                    "(k_shift=%u k_width=%u) -> kdiv=%d\n",
                    clk->pll_con1, pll->k_shift, pll->k_width, kdiv);

            num = ((unsigned long long)mdiv << 16) + (u16)kdiv;
            pr_info("CMUCAL: ra_get_pll: type=FRAC: fin=%llu * "
                    "((mdiv<<16)+kdiv)=%llu / (pdiv=%u << sdiv=%u) then >>16\n",
                    fout, num, pdiv, sdiv);

            fout *= num;
            do_div(fout, (pdiv << sdiv));
            fout >>= 16;

            pr_info("CMUCAL: ra_get_pll: type=FRAC: fin=%llu -> fout=%llu\n",
                    before, fout);
        } else {
            pr_err("CMUCAL: ra_get_pll: ERROR unsupported PLL type or missing "
                   "pll_con1 (pll=%p pll_con1=%p)\n",
                   pll, clk->pll_con1);
            fout = 0;
        }

        pr_info("CMUCAL: ra_get_pll: exit fout=%llu (truncated return=%u)\n",
                fout, (unsigned int)fout);
        return (unsigned int)fout;
    }

    static unsigned int ra_get_pll_idx(struct cmucal_clk * clk) {
        struct cmucal_pll *pll;
        struct cmucal_pll_table *prate_table;
        unsigned int mdiv, pdiv, sdiv, pll_con0;
        int i;

        if (!clk) {
            pr_info("CMUCAL: ra_get_pll_idx: ERROR clk is NULL -> return "
                    "(unsigned)-1\n");
            return (unsigned int)-1;
        }

        pll = to_pll_clk(clk);
        prate_table = pll ? pll->rate_table : NULL;

        pr_info("CMUCAL: ra_get_pll_idx: enter clk=%p pid=%u pll=%p "
                "rate_table=%p rate_count=%d pll_con0=%p\n",
                clk, clk->pid, pll, prate_table, pll ? pll->rate_count : -1,
                clk->pll_con0);

        if (!pll || !prate_table || pll->rate_count <= 0) {
            pr_info("CMUCAL: ra_get_pll_idx: ERROR invalid pll/table/count -> "
                    "show pll=%p table=%p count=%d\n",
                    pll, prate_table, pll ? pll->rate_count : -1);
            return (unsigned int)-1;
        }

        pll_con0 = readl(clk->pll_con0);
        mdiv = (pll_con0 >> pll->m_shift) & width_to_mask(pll->m_width);
        pdiv = (pll_con0 >> pll->p_shift) & width_to_mask(pll->p_width);
        sdiv = (pll_con0 >> pll->s_shift) & width_to_mask(pll->s_width);

        pr_info("CMUCAL: ra_get_pll_idx: pll_con0=0x%08x -> mdiv=%u pdiv=%u "
                "sdiv=%u\n",
                pll_con0, mdiv, pdiv, sdiv);

        /* Optional: dump a handful of table rows for sanity */
        for (i = 0; i < pll->rate_count && i < 8; i++)
            pr_info("CMUCAL: ra_get_pll_idx: table[%d]=%p rate=%lu "
                    "m/p/s=%u/%u/%u\n",
                    i, &prate_table[i], prate_table[i].rate,
                    prate_table[i].mdiv, prate_table[i].pdiv,
                    prate_table[i].sdiv);

        for (i = 0; i < pll->rate_count; i++) {
            if (mdiv != prate_table[i].mdiv)
                continue;
            if (pdiv != prate_table[i].pdiv)
                continue;
            if (sdiv != prate_table[i].sdiv)
                continue;

            pr_info("CMUCAL: ra_get_pll_idx: HIT i=%d entry=%p rate=%lu "
                    "m/p/s=%u/%u/%u\n",
                    i, &prate_table[i], prate_table[i].rate,
                    prate_table[i].mdiv, prate_table[i].pdiv,
                    prate_table[i].sdiv);
            return i;
        }

        pr_info("CMUCAL: ra_get_pll_idx: MISS: no matching table entry for "
                "m/p/s=%u/%u/%u\n",
                mdiv, pdiv, sdiv);

        return (unsigned int)-1;
    }

    static int ra_set_gate(struct cmucal_clk * clk, unsigned int pass) {
        unsigned int reg;

        /*
         * MANUAL(status) 1 : CG_VALUE(offset) control
         *                0 : ENABLE_AUTOMATIC_CLKGATING(enable) control
         */
        if (!clk->status || get_bit(clk->status, clk->s_shift)) {
            reg = readl(clk->offset);
            reg &= ~(get_mask(clk->width, clk->shift));
            if (pass)
                reg |= get_mask(clk->width, clk->shift);

            writel(reg, clk->offset);
        } else {
            reg = readl(clk->enable);
            reg &= ~(get_mask(clk->e_width, clk->e_shift));
            if (!pass)
                reg |= get_mask(clk->e_width, clk->e_shift);

            writel(reg, clk->offset);
        }
        return 0;
    }

    static unsigned int ra_get_gate(struct cmucal_clk * clk) {
        unsigned int pass;

        /*
         * MANUAL(status) 1 : CG_VALUE(offset) control
         *                0 : ENABLE_AUTOMATIC_CLKGATING(enable) control
         */
        if (!clk->status || get_bit(clk->status, clk->s_shift))
            pass = get_value(clk->offset, clk->shift, clk->width);
        else
            pass = !(get_value(clk->enable, clk->e_shift, clk->e_width));

        return pass;
    }

    /*
     * en : qch enable bit
     * req : qch request bit
     * expire == 0 => default value
     * expire != 0 => change value
     */
    int ra_set_qch(unsigned int id, unsigned int en, unsigned int req,
                   unsigned int expire) {
        struct cmucal_qch *qch;
        struct cmucal_clk *clk;
        unsigned int reg;

        clk = cmucal_get_node(id);
        if (!clk) {
            pr_err("%s:[%x]\n", __func__, id);
            return -EVCLKINVAL;
        }

        if (!IS_QCH(clk->id)) {
            if (IS_GATE(clk->id)) {
                reg = readl(clk->status);
                reg &= ~(get_mask(clk->s_width, clk->s_shift));
                if (!en)
                    reg |= get_mask(clk->s_width, clk->s_shift);

                writel(reg, clk->status);
                return 0;
            }

            pr_err("%s:cannot find qch [%x]\n", __func__, id);
            return -EVCLKINVAL;
        }

        if (expire) {
            reg = ((en & width_to_mask(clk->width)) << clk->shift) |
                  ((req & width_to_mask(clk->s_width)) << clk->s_shift) |
                  ((expire & width_to_mask(clk->e_width)) << clk->e_shift);
        } else {
            reg = readl(clk->offset);
            reg &= ~(get_mask(clk->width, clk->shift) |
                     get_mask(clk->s_width, clk->s_shift));
            reg |= (en << clk->shift) | (req << clk->s_shift);
        }

        if (IS_ENABLED(CONFIG_CMUCAL_QCH_IGNORE_SUPPORT)) {
            qch = to_qch(clk);

            if (en)
                reg &= ~(0x1 << qch->ig_shift);
            else
                reg |= (0x1 << qch->ig_shift);
        }

        writel(reg, clk->offset);

        return 0;
    }

    static int ra_req_enable_qch(struct cmucal_clk * clk, unsigned int req) {
        unsigned int reg;
        /*
         * QH ENABLE(offset) 1 : Skip
         *		     0 : REQ(status) control
         */
        if (!get_bit(clk->offset, clk->shift)) {
            reg = readl(clk->status);
            reg &= ~(get_mask(clk->s_width, clk->s_shift));
            if (req)
                reg |= get_mask(clk->s_width, clk->s_shift);

            writel(reg, clk->status);
        }

        return 0;
    }

    int ra_enable_qch(struct cmucal_clk * clk, unsigned int en) {
        unsigned int reg;
        /*
         * QH ENABLE(offset)
         */
        reg = readl(clk->offset);
        reg &= ~(get_mask(clk->width, clk->shift));
        if (en)
            reg |= get_mask(clk->width, clk->shift);

        writel(reg, clk->offset);

        return 0;
    }

    int ra_set_enable_hwacg(struct cmucal_clk * clk, unsigned int en) {
        unsigned int reg;

        /*
         * Automatic clkgating enable(enable)
         */
        if (!clk->enable)
            return 0;

        reg = readl(clk->enable);
        reg &= ~(get_mask(clk->e_width, clk->e_shift));
        if (en)
            reg |= get_mask(clk->s_width, clk->s_shift);

        writel(reg, clk->enable);

        return 0;
    }

    static int ra_enable_fixed_rate(struct cmucal_clk * clk,
                                    unsigned int params) {
        unsigned int reg;
        void __iomem *offset;
        int ret;

        if (!clk->enable)
            return 0;

        offset = convert_pll_base(clk->enable);
        reg = readl(offset);
        if (params) {
            reg |= (PLL_ENABLE | PLL_MUX_SEL);
            writel(reg, offset);

            ret = ra_wait_done(offset, PLL_STABLE_SHIFT, 1, 400);
            if (ret)
                pr_err("fixed pll enable time out, \'%s\'\n", clk->name);
        } else {
            reg &= ~(PLL_MUX_SEL);
            writel(reg, offset);
            ret = ra_wait_done(offset, PLL_MUX_BUSY_SHIFT, 0, 100);
            if (ret)
                pr_err("fixed pll mux change time out, \'%s\'\n", clk->name);

            reg &= ~(PLL_ENABLE);
            writel(reg, offset);
        }

        return 0;
    }

    int ra_enable_clkout(struct cmucal_clk * clk, bool enable) {
        struct cmucal_clkout *clkout = to_clkout(clk);

        if (enable) {
            exynos_pmu_update(clk->offset_idx, get_mask(clk->width, clk->shift),
                              clkout->sel << clk->shift);
            exynos_pmu_update(clk->offset_idx,
                              get_mask(clk->e_width, clk->e_shift),
                              0x0 << clk->e_shift);
        } else {
            exynos_pmu_update(clk->offset_idx,
                              get_mask(clk->e_width, clk->e_shift),
                              0x1 << clk->e_shift);
        }

        return 0;
    }

    int ra_set_enable(unsigned int id, unsigned int params) {
        struct cmucal_clk *clk;
        unsigned type = GET_TYPE(id);
        int ret = 0;

        clk = cmucal_get_node(id);
        if (!clk) {
            pr_err("%s:[%x]type : %x, params : %x\n", __func__, id, type,
                   params);
            return -EVCLKINVAL;
        }

        switch (type) {
        case FIXED_RATE_TYPE:
            ret = ra_enable_fixed_rate(clk, params);
            break;
        case PLL_TYPE:
            ret = ra_enable_pll(clk, params);
            break;
        case MUX_TYPE:
            if (IS_USER_MUX(clk->id))
                ret = ra_set_div_mux(clk, params);
            break;
        case GATE_TYPE:
            ret = ra_set_gate(clk, params);
            break;
        case QCH_TYPE:
            ret = ra_req_enable_qch(clk, params);
            break;
        case DIV_TYPE:
            break;
        case CLKOUT_TYPE:
            ret = ra_enable_clkout(clk, params);
            break;
        default:
            pr_err("Un-support clk type %x\n", id);
            ret = -EVCLKINVAL;
        }

        return ret;
    }

    int ra_set_value(unsigned int id, unsigned int params) {
        struct cmucal_clk *clk;
        unsigned int type = GET_TYPE(id);
        int ret;
        unsigned int after = 0;
        struct ra_log_ctx log;

        clk = cmucal_get_node(id);
        log = ra_log_push_clk(clk, id);

        pr_info("RA: ra_set_value: enter id=0x%x type=0x%x params=0x%x\n", id,
                type, params);

        pr_info("RA: ra_set_value: cmucal_get_node(id=0x%x) -> clk=%p\n", id,
                clk);

        if (!clk) {
            pr_err("RA: ra_set_value: ERROR clk NULL id=0x%x type=0x%x "
                   "params=0x%x\n",
                   id, type, params);
            ret = -EVCLKINVAL;
            goto out;
        }

        pr_info("RA: ra_set_value: clk name=%s id=0x%x type=0x%x params=0x%x\n",
                clk->name, id, type, params);

        switch (type) {
        case DIV_TYPE:
            pr_info("RA: ra_set_value: DIV_TYPE -> ra_set_div_mux(clk=%p, "
                    "params=0x%x)\n",
                    clk, params);
            ret = ra_set_div_mux(clk, params);
            break;

        case MUX_TYPE:
            pr_info("RA: ra_set_value: MUX_TYPE -> ra_set_div_mux(clk=%p, "
                    "params=0x%x)\n",
                    clk, params);
            ret = ra_set_div_mux(clk, params);
            break;

        case PLL_TYPE:
            pr_info("RA: ra_set_value: PLL_TYPE -> ra_set_pll(clk=%p, "
                    "params=0x%x, enable=0)\n",
                    clk, params);
            ret = ra_set_pll(clk, params, 0);
            break;

        case GATE_TYPE:
            pr_info("RA: ra_set_value: GATE_TYPE -> ra_set_gate(clk=%p, "
                    "params=0x%x)\n",
                    clk, params);
            ret = ra_set_gate(clk, params);
            break;

        default:
            pr_err("RA: ra_set_value: ERROR unsupported clk type id=0x%x "
                   "type=0x%x\n",
                   id, type);
            ret = -EVCLKINVAL;
            break;
        }

        pr_info("RA: ra_set_value: op done id=0x%x type=0x%x ret=%d\n", id,
                type, ret);

        /* Optional but very helpful: read back to confirm the set took effect
         */
        if (!ret) {
            after = ra_get_value(id);
            pr_info("RA: ra_set_value: readback id=0x%x type=0x%x -> val=0x%x "
                    "(params was 0x%x)\n",
                    id, type, after, params);
        }

        pr_info("RA: ra_set_value: exit id=0x%x ret=%d\n", id, ret);
    out:
        ra_log_pop(log);
        return ret;
    }

    unsigned int ra_get_value(unsigned int id) {
        struct cmucal_clk *clk;
        unsigned int type = GET_TYPE(id);
        unsigned int val = 0;
        struct ra_log_ctx log;

        clk = cmucal_get_node(id);
        log = ra_log_push_clk(clk, id);

        pr_info("RA: ra_get_value: enter id=0x%x type=0x%x\n", id, type);

        pr_info("RA: ra_get_value: cmucal_get_node(id=0x%x) -> clk=%p\n", id,
                clk);

        if (!clk) {
            pr_err("RA: ra_get_value: ERROR clk NULL id=0x%x type=0x%x\n", id,
                   type);
            goto out;
        }

        pr_info("RA: ra_get_value: clk name=%s id=0x%x type=0x%x\n", clk->name,
                id, type);

        switch (type) {
        case DIV_TYPE:
            pr_info("RA: ra_get_value: DIV_TYPE -> ra_get_div_mux(clk=%p)\n",
                    clk);
            val = ra_get_div_mux(clk);
            break;

        case MUX_TYPE:
            pr_info("RA: ra_get_value: MUX_TYPE -> ra_get_div_mux(clk=%p)\n",
                    clk);
            val = ra_get_div_mux(clk);
            break;

        case PLL_TYPE:
            pr_info("RA: ra_get_value: PLL_TYPE -> ra_get_pll(clk=%p)\n", clk);
            val = ra_get_pll(clk);
            break;

        case GATE_TYPE:
            pr_info("RA: ra_get_value: GATE_TYPE -> ra_get_gate(clk=%p)\n",
                    clk);
            val = ra_get_gate(clk);
            break;

        case FIXED_RATE_TYPE:
            pr_info("RA: ra_get_value: FIXED_RATE_TYPE -> "
                    "ra_get_fixed_rate(clk=%p)\n",
                    clk);
            val = ra_get_fixed_rate(clk);
            break;

        case FIXED_FACTOR_TYPE:
            pr_info("RA: ra_get_value: FIXED_FACTOR_TYPE -> "
                    "ra_get_fixed_factor(clk=%p)\n",
                    clk);
            val = ra_get_fixed_factor(clk);
            break;

        default:
            pr_err("RA: ra_get_value: ERROR unsupported clk type id=0x%x "
                   "type=0x%x\n",
                   id, type);
            val = 0;
            break;
        }

        pr_info("RA: ra_get_value: exit id=0x%x type=0x%x val=0x%x\n", id, type,
                val);
    out:
        ra_log_pop(log);
        return val;
    }

    static unsigned int __init ra_get_sfr_address(
        unsigned short idx, void __iomem **addr, unsigned char *shift,
        unsigned char *width) {
        struct sfr_access *field;
        struct sfr *reg;
        struct sfr_block *block;

        field = cmucal_get_sfr_node(idx | SFR_ACCESS_TYPE);
        if (!field) {
            pr_info("%s:failed idx:%x\n", __func__, idx);
            return 0;
        }
        *shift = field->shift;
        *width = field->width;

        reg = cmucal_get_sfr_node(field->sfr | SFR_TYPE);
        if (!reg) {
            pr_info("%s:failed idx:%x sfr:%x\n", __func__, idx, field->sfr);
            return 0;
        }

        block = cmucal_get_sfr_node(reg->block | SFR_BLOCK_TYPE);
        if (!reg || !block) {
            pr_info("%s:failed idx:%x reg:%x\n", __func__, idx, reg->block);
            return 0;
        }
        *addr = block->va + reg->offset;

        return block->pa + reg->offset;
    }

    static void ra_get_pll_address(struct cmucal_clk * clk) {
        struct cmucal_pll *pll;

        pr_info("RA: ra_get_pll_address: enter clk=%p name=%s offset_idx=%u "
                "enable_idx=%u status_idx=%u\n",
                clk, clk ? clk->name : "(null)", clk ? clk->offset_idx : 0,
                clk ? clk->enable_idx : 0, clk ? clk->status_idx : 0);

        if (!clk) {
            pr_info("RA: ra_get_pll_address: clk NULL -> return\n");
            return;
        }

        pll = to_pll_clk(clk);
        pr_info("RA: ra_get_pll_address: to_pll_clk(%p) -> pll=%p m_idx=%u "
                "p_idx=%u s_idx=%u k_idx=%u\n",
                clk, pll, pll ? pll->m_idx : 0, pll ? pll->p_idx : 0,
                pll ? pll->s_idx : 0, pll ? pll->k_idx : 0);

        /* lock_div */
        pr_info("RA: ra_get_pll_address: lock_div: ra_get_sfr_address(idx=%u, "
                "&lock, &shift, &width)\n",
                clk->offset_idx);
        ra_get_sfr_address(clk->offset_idx, &clk->lock, &clk->shift,
                           &clk->width);
        pr_info("RA: ra_get_pll_address: lock_div result: lock=%p shift=%u "
                "width=%u\n",
                clk->lock, clk->shift, clk->width);

        /* enable_div */
        pr_info("RA: ra_get_pll_address: enable_div: "
                "ra_get_sfr_address(idx=%u, &pll_con0, &e_shift, &e_width)\n",
                clk->enable_idx);
        clk->paddr = ra_get_sfr_address(clk->enable_idx, &clk->pll_con0,
                                        &clk->e_shift, &clk->e_width);
        pr_info("RA: ra_get_pll_address: enable_div result: paddr=%p "
                "pll_con0=%p e_shift=%u e_width=%u\n",
                clk->paddr, clk->pll_con0, clk->e_shift, clk->e_width);

        /* status_div */
        pr_info("RA: ra_get_pll_address: status_div: "
                "ra_get_sfr_address(idx=%u, &pll_con0, &s_shift, &s_width)\n",
                clk->status_idx);
        ra_get_sfr_address(clk->status_idx, &clk->pll_con0, &clk->s_shift,
                           &clk->s_width);
        pr_info("RA: ra_get_pll_address: status_div result: pll_con0=%p "
                "s_shift=%u s_width=%u\n",
                clk->pll_con0, clk->s_shift, clk->s_width);

        /* m_div */
        if (pll) {
            pr_info("RA: ra_get_pll_address: m_div: ra_get_sfr_address(idx=%u, "
                    "&pll_con0, &m_shift, &m_width)\n",
                    pll->m_idx);
            ra_get_sfr_address(pll->m_idx, &clk->pll_con0, &pll->m_shift,
                               &pll->m_width);
            pr_info("RA: ra_get_pll_address: m_div result: pll_con0=%p "
                    "m_shift=%u m_width=%u\n",
                    clk->pll_con0, pll->m_shift, pll->m_width);

            /* p_div */
            pr_info("RA: ra_get_pll_address: p_div: ra_get_sfr_address(idx=%u, "
                    "&pll_con0, &p_shift, &p_width)\n",
                    pll->p_idx);
            ra_get_sfr_address(pll->p_idx, &clk->pll_con0, &pll->p_shift,
                               &pll->p_width);
            pr_info("RA: ra_get_pll_address: p_div result: pll_con0=%p "
                    "p_shift=%u p_width=%u\n",
                    clk->pll_con0, pll->p_shift, pll->p_width);

            /* s_div */
            pr_info("RA: ra_get_pll_address: s_div: ra_get_sfr_address(idx=%u, "
                    "&pll_con0, &s_shift, &s_width)\n",
                    pll->s_idx);
            ra_get_sfr_address(pll->s_idx, &clk->pll_con0, &pll->s_shift,
                               &pll->s_width);
            pr_info("RA: ra_get_pll_address: s_div result: pll_con0=%p "
                    "s_shift=%u s_width=%u\n",
                    clk->pll_con0, pll->s_shift, pll->s_width);

            /* k_div */
            if (pll->k_idx != EMPTY_CAL_ID) {
                pr_info(
                    "RA: ra_get_pll_address: k_div: ra_get_sfr_address(idx=%u, "
                    "&pll_con1, &k_shift, &k_width)\n",
                    pll->k_idx);
                ra_get_sfr_address(pll->k_idx, &clk->pll_con1, &pll->k_shift,
                                   &pll->k_width);
                pr_info("RA: ra_get_pll_address: k_div result: pll_con1=%p "
                        "k_shift=%u k_width=%u\n",
                        clk->pll_con1, pll->k_shift, pll->k_width);
            } else {
                clk->pll_con1 = NULL;
                pr_info("RA: ra_get_pll_address: k_div skipped "
                        "(k_idx==EMPTY_CAL_ID) -> pll_con1=NULL\n");
            }
        }

        pr_info("RA: ra_get_pll_address: exit name=%s paddr=%p lock=%p "
                "pll_con0=%p pll_con1=%p\n",
                clk->name, clk->paddr, clk->lock, clk->pll_con0, clk->pll_con1);
    }

    static void ra_get_pll_rate_table(struct cmucal_clk * clk) {
        struct cmucal_pll *pll;
        void *pll_block;
        struct cmucal_pll_table *table;
        struct ect_pll *pll_unit;
        struct ect_pll_frequency *pll_frequency;
        int i;

        pr_info("RA: ra_get_pll_rate_table: enter clk=%p name=%s\n", clk,
                clk ? clk->name : "(null)");

        if (!clk) {
            pr_info("RA: ra_get_pll_rate_table: clk NULL -> return\n");
            return;
        }

        pll = to_pll_clk(clk);
        pr_info("RA: ra_get_pll_rate_table: to_pll_clk(%p) -> pll=%p (old "
                "rate_table=%p old rate_count=%u)\n",
                clk, pll, pll ? pll->rate_table : NULL,
                pll ? pll->rate_count : 0);

        pll_block = ect_get_block(BLOCK_PLL);
        pr_info("RA: ra_get_pll_rate_table: ect_get_block(BLOCK_PLL) -> %p\n",
                pll_block);
        if (!pll_block) {
            pr_info("RA: ra_get_pll_rate_table: no pll_block -> return\n");
            return;
        }

        pll_unit = ect_pll_get_pll(pll_block, clk->name);
        pr_info("RA: ra_get_pll_rate_table: ect_pll_get_pll(block=%p, name=%s) "
                "-> pll_unit=%p\n",
                pll_block, clk->name, pll_unit);
        if (!pll_unit) {
            pr_info("RA: ra_get_pll_rate_table: no pll_unit for name=%s -> "
                    "return\n",
                    clk->name);
            return;
        }

        pr_info("RA: ra_get_pll_rate_table: pll_unit num_of_frequency=%u "
                "frequency_list=%p\n",
                pll_unit->num_of_frequency, pll_unit->frequency_list);

        table = kzalloc(sizeof(struct cmucal_pll_table) *
                            pll_unit->num_of_frequency,
                        GFP_KERNEL);
        pr_info("RA: ra_get_pll_rate_table: kzalloc(sz=%zu) -> table=%p\n",
                sizeof(struct cmucal_pll_table) *
                    (size_t)pll_unit->num_of_frequency,
                table);
        if (!table) {
            pr_info("RA: ra_get_pll_rate_table: kzalloc failed -> return\n");
            return;
        }

        for (i = 0; i < pll_unit->num_of_frequency; ++i) {
            pll_frequency = &pll_unit->frequency_list[i];

            table[i].rate = pll_frequency->frequency;
            table[i].pdiv = pll_frequency->p;
            table[i].mdiv = pll_frequency->m;
            table[i].sdiv = pll_frequency->s;
            table[i].kdiv = pll_frequency->k;

            pr_info("RA: ra_get_pll_rate_table: [%d] freq=%u p=%u m=%u s=%u "
                    "k=%d -> table{rate=%u pdiv=%u mdiv=%u sdiv=%u kdiv=%d}\n",
                    i, pll_frequency->frequency, pll_frequency->p,
                    pll_frequency->m, pll_frequency->s, pll_frequency->k,
                    table[i].rate, table[i].pdiv, table[i].mdiv, table[i].sdiv,
                    table[i].kdiv);
        }

        if (pll) {
            pll->rate_table = table;
            pll->rate_count = pll_unit->num_of_frequency;
            pr_info("RA: ra_get_pll_rate_table: assigned pll->rate_table=%p "
                    "pll->rate_count=%u\n",
                    pll->rate_table, pll->rate_count);
        } else {
            /* Defensive: avoid leaking table if pll is unexpectedly NULL */
            pr_info("RA: ra_get_pll_rate_table: ERROR pll is NULL, freeing "
                    "table=%p\n",
                    table);
            kfree(table);
        }

        pr_info("RA: ra_get_pll_rate_table: exit name=%s\n", clk->name);
    }

    int ra_set_list_enable(unsigned int *list, unsigned int num_list) {
        unsigned int id;
        int i;

        for (i = 0; i < num_list; i++) {
            id = list[i];
            if (IS_USER_MUX(id) || IS_GATE(id))
                ra_set_value(id, 1);
            else if (IS_PLL(id))
                ra_set_enable(id, 1);
        }

        return 0;
    }
    EXPORT_SYMBOL_GPL(ra_set_list_enable);

    int ra_set_list_disable(unsigned int *list, unsigned int num_list) {
        unsigned int id;
        int i;

        for (i = num_list; i > 0; i--) {
            id = list[i - 1];
            if (IS_USER_MUX(id) || IS_GATE(id))
                ra_set_value(id, 0);
            else if (IS_PLL(id))
                ra_set_enable(id, 0);
        }

        return 0;
    }
    EXPORT_SYMBOL_GPL(ra_set_list_disable);

    void ra_set_pll_ops(unsigned int *list, struct vclk_lut *lut,
                        unsigned int num_list, struct vclk_trans_ops *ops) {
        unsigned int from, to;
        int i;
        bool trans;

        for (i = 0; i < num_list; i++) {
            if (GET_TYPE(list[i]) != PLL_TYPE)
                continue;

            to = lut->params[i];
            if (ops && ops->get_pll)
                from = ops->get_pll(list[i]);
            else
                from = ra_get_value(list[i]);

            trans = ra_get_trans_opt(to, from);
            if (trans == TRANS_IGNORE)
                continue;

            if (ops && ops->set_pll)
                ops->set_pll(list[i], to);
            else
                ra_set_value(list[i], to);
        }
    }
    EXPORT_SYMBOL_GPL(ra_set_pll_ops);

    void ra_set_clk_by_type(unsigned int *list, struct vclk_lut *lut,
                            unsigned int num_list, unsigned int type,
                            enum trans_opt opt) {
        unsigned int from, to;
        int i;
        bool trans;

        for (i = 0; i < num_list; i++) {
            if (GET_TYPE(list[i]) != type)
                continue;

            to = lut->params[i];
            from = ra_get_value(list[i]);
            trans = ra_get_trans_opt(to, from);
            if (trans == TRANS_IGNORE)
                continue;
            if (opt != TRANS_FORCE && trans != opt)
                continue;

            ra_set_value(list[i], to);
        }
    }
    EXPORT_SYMBOL_GPL(ra_set_clk_by_type);

    void ra_set_clk_by_seq(unsigned int *list, struct vclk_lut *lut,
                           struct vclk_seq *seq, unsigned int num_list) {
        unsigned int from, to;
        unsigned int i, idx;
        bool trans;

        for (i = 0; i < num_list; i++) {
            from = lut->params[i];
            to = ra_get_value(list[i]);
            trans = ra_get_trans_opt(to, from);
            if (seq[i].opt & trans) {
                idx = seq[i].idx;
                ra_set_value(list[idx], to);
            }
        }
    }
    EXPORT_SYMBOL_GPL(ra_set_clk_by_seq);

    int ra_compare_clk_list(unsigned int *params, unsigned int *list,
                            unsigned int num_list) {
        struct cmucal_clk *clk;
        unsigned int i, type;
        unsigned int expected, actual;
        bool has_g3d = ra_list_has_g3d(list, num_list);
        struct ra_log_ctx log = ra_log_push(has_g3d ? 10 : 0, NULL);

        pr_info(
            "RA: ra_compare_clk_list: enter params=%p list=%p num_list=%u\n",
            params, list, num_list);

        if (!params || !list) {
            pr_err(
                "RA: ra_compare_clk_list: ERROR NULL input params=%p list=%p\n",
                params, list);
            ra_log_pop(log);
            return -EVCLKINVAL;
        }

        for (i = 0; i < num_list; i++) {
            type = GET_TYPE(list[i]);

            pr_info("RA: ra_compare_clk_list: idx=%u id=0x%x type=0x%x\n", i,
                    list[i], type);

            clk = cmucal_get_node(list[i]);
            pr_info("RA: ra_compare_clk_list: idx=%u cmucal_get_node(id=0x%x) "
                    "-> clk=%p\n",
                    i, list[i], clk);

            if (!clk) {
                pr_err("RA: ra_compare_clk_list: ERROR clk NULL idx=%u id=0x%x "
                       "type=0x%x\n",
                       i, list[i], type);
                ra_log_pop(log);
                return -EVCLKINVAL;
            }

            expected = params[i];
            pr_info(
                "RA: ra_compare_clk_list: idx=%u clk name=%s expected=0x%x\n",
                i, clk->name, expected);

            switch (type) {
            case DIV_TYPE:
                actual = ra_get_div_mux(clk);
                pr_info("RA: ra_compare_clk_list: idx=%u DIV_TYPE "
                        "actual(ra_get_div_mux)=0x%x\n",
                        i, actual);
                break;

            case MUX_TYPE:
                actual = ra_get_div_mux(clk);
                pr_info("RA: ra_compare_clk_list: idx=%u MUX_TYPE "
                        "actual(ra_get_div_mux)=0x%x\n",
                        i, actual);
                break;

            case PLL_TYPE:
                actual = ra_get_pll_idx(clk);
                pr_info("RA: ra_compare_clk_list: idx=%u PLL_TYPE "
                        "actual(ra_get_pll_idx)=0x%x\n",
                        i, actual);
                break;

            default:
                pr_err("RA: ra_compare_clk_list: ERROR unsupported clk type "
                       "idx=%u id=0x%x type=0x%x\n",
                       i, list[i], type);
                ra_log_pop(log);
                return -EVCLKINVAL;
            }

            if (expected != actual) {
                pr_info("RA: ra_compare_clk_list: MISMATCH idx=%u name=%s "
                        "id=0x%x type=0x%x expected=0x%x actual=0x%x\n",
                        i, clk->name, list[i], type, expected, actual);
                goto mismatch;
            }

            pr_info("RA: ra_compare_clk_list: MATCH idx=%u name=%s id=0x%x "
                    "expected=0x%x\n",
                    i, clk->name, list[i], expected);
        }

        pr_info("RA: ra_compare_clk_list: exit OK\n");
        ra_log_pop(log);
        return 0;

    mismatch:
        /*
         * Optional: also show ra_get_value() for cross-checking the generic
         * accessor, but keep it separate so you can see if ra_get_value()
         * disagrees with the type-specific accessor used above.
         */
        pr_info(
            "RA: ra_compare_clk_list: mismatch summary: idx=%u name=%s id=0x%x "
            "expected=0x%x type_specific_actual=0x%x ra_get_value=0x%x\n",
            i, clk->name, list[i], expected, actual, ra_get_value(list[i]));

        ra_log_pop(log);
        return -EVCLKNOENT;
    }
    EXPORT_SYMBOL_GPL(ra_compare_clk_list);

    unsigned int ra_set_rate_switch(struct vclk_switch * info,
                                    unsigned int rate_max) {
        struct switch_lut *lut;
        unsigned int switch_rate = rate_max;
        int i;

        for (i = 0; i < info->num_switches; i++) {
            lut = &info->lut[i];
            if (rate_max >= lut->rate) {
                if (info->src_div != EMPTY_CLK_ID)
                    ra_set_value(info->src_div, lut->div_value);
                if (info->src_mux != EMPTY_CLK_ID)
                    ra_set_value(info->src_mux, lut->mux_value);

                switch_rate = lut->rate;
                break;
            }
        }

        if (i == info->num_switches)
            switch_rate = rate_max;

        return switch_rate;
    }
    EXPORT_SYMBOL_GPL(ra_set_rate_switch);

    void ra_select_switch_pll(struct vclk_switch * info, unsigned int value) {
        if (value) {
            if (info->src_gate != EMPTY_CLK_ID)
                ra_set_value(info->src_gate, value);
            if (info->src_umux != EMPTY_CLK_ID)
                ra_set_value(info->src_umux, value);
        }

        ra_set_value(info->switch_mux, value);

        if (!value) {
            if (info->src_umux != EMPTY_CLK_ID)
                ra_set_value(info->src_umux, value);
            if (info->src_gate != EMPTY_CLK_ID)
                ra_set_value(info->src_gate, value);
        }
    }
    EXPORT_SYMBOL_GPL(ra_select_switch_pll);

    struct cmucal_clk *ra_get_parent(unsigned int id) {
        struct cmucal_clk *clk, *parent;
        struct cmucal_mux *mux;
        unsigned int val;

        clk = cmucal_get_node(id);
        if (!clk)
            return NULL;

        switch (GET_TYPE(clk->id)) {
        case FIXED_RATE_TYPE:
        case FIXED_FACTOR_TYPE:
        case PLL_TYPE:
        case DIV_TYPE:
        case GATE_TYPE:
            if (clk->pid == EMPTY_CLK_ID)
                parent = NULL;
            else
                parent = cmucal_get_node(clk->pid);
            break;
        case MUX_TYPE:
            mux = to_mux_clk(clk);
            val = ra_get_div_mux(clk);
            parent = cmucal_get_node(mux->pid[val]);
            break;
        default:
            parent = NULL;
            break;
        }

        return parent;
    }
    EXPORT_SYMBOL_GPL(ra_get_parent);

    int ra_set_rate(unsigned int id, unsigned int rate) {
        struct cmucal_clk *clk;
        int ret = 0;
        struct ra_log_ctx log;

        clk = cmucal_get_node(id);
        log = ra_log_push_clk(clk, id);
        if (!clk) {
            ra_log_pop(log);
            return -EVCLKINVAL;
        }

        switch (GET_TYPE(clk->id)) {
        case PLL_TYPE:
            ret = ra_set_pll(clk, rate / 1000, rate);
            break;
        case DIV_TYPE:
            ret = ra_set_div_rate(clk, rate);
            break;
        case MUX_TYPE:
            ret = ra_set_mux_rate(clk, rate);
            break;
        default:
            pr_err("Un-support clk type %x, rate = %u\n", id, rate);
            ret = -EVCLKINVAL;
            break;
        }

        ra_log_pop(log);

        return ret;
    }
    EXPORT_SYMBOL_GPL(ra_set_rate);

    unsigned int ra_recalc_rate(unsigned int id) {
        struct cmucal_clk *clk;
        unsigned int cur;
        unsigned int clk_path[RECALC_MAX];
        unsigned int depth, ratio;
        unsigned long rate;
        struct ra_log_ctx log;

        clk = cmucal_get_node(id);
        log = ra_log_push_clk(clk, id);

        pr_info("RA: ra_recalc_rate: enter id=0x%x type=0x%x\n", id,
                GET_TYPE(id));

        if (GET_TYPE(id) > GATE_TYPE) {
            pr_info("RA: ra_recalc_rate: id=0x%x type=0x%x > GATE_TYPE -> "
                    "return 0\n",
                    id, GET_TYPE(id));
            rate = 0;
            goto out;
        }

        cur = id;
        pr_info("RA: ra_recalc_rate: start walk cur=0x%x\n", cur);

        for (depth = 0; depth < RECALC_MAX; depth++) {
            clk_path[depth] = cur;

            clk = ra_get_parent(cur);
            pr_info("RA: ra_recalc_rate: depth=%u cur=0x%x ra_get_parent(cur) "
                    "-> clk=%p\n",
                    depth, cur, clk);

            if (!clk) {
                pr_info("RA: ra_recalc_rate: stop walk (no parent) at depth=%u "
                        "cur=0x%x\n",
                        depth, cur);
                break;
            }

            pr_info("RA: ra_recalc_rate: parent: name=%s id=0x%x (next cur)\n",
                    clk->name, clk->id);

            cur = clk->id;
        }

        if (depth == RECALC_MAX) {
            pr_err(
                "RA: ra_recalc_rate: ERROR overflow id=0x%x (RECALC_MAX=%d)\n",
                id, RECALC_MAX);
            rate = 0;
            goto out;
        }

        /* Dump the path we collected */
        pr_info(
            "RA: ra_recalc_rate: collected depth=%u path (leaf->root-ish):\n",
            depth);
        for (cur = 0; cur <= depth; cur++)
            pr_info("RA: ra_recalc_rate:   path[%u]=0x%x type=0x%x\n", cur,
                    clk_path[cur], GET_TYPE(clk_path[cur]));

        /*
         * get root clock rate
         * - if the node just below the last collected is PLL, use that PLL node
         * - else use the last collected node
         */
        if (depth > 0 && IS_PLL(clk_path[depth - 1])) {
            pr_info("RA: ra_recalc_rate: root selection: depth=%u and "
                    "path[%u]=0x%x is PLL -> use PLL node\n",
                    depth, depth - 1, clk_path[depth - 1]);
            rate = ra_get_value(clk_path[--depth]);
            pr_info("RA: ra_recalc_rate: initial rate from PLL node id=0x%x -> "
                    "rate=%lu\n",
                    clk_path[depth], rate);
        } else {
            pr_info("RA: ra_recalc_rate: root selection: use path[%u]=0x%x\n",
                    depth, clk_path[depth]);
            rate = ra_get_value(clk_path[depth]);
            pr_info("RA: ra_recalc_rate: initial rate from node id=0x%x -> "
                    "rate=%lu\n",
                    clk_path[depth], rate);
        }

        if (!rate) {
            pr_info("RA: ra_recalc_rate: initial rate is 0 -> return 0\n");
            goto out;
        }

        /* calc request clock node rate */
        for (; depth > 0; --depth) {
            cur = clk_path[depth - 1];

            if (IS_FIXED_FACTOR(cur) || IS_DIV(cur)) {
                unsigned int raw = ra_get_value(cur);
                ratio = raw + 1;

                pr_info("RA: ra_recalc_rate: apply divide: node id=0x%x "
                        "type=0x%x raw=0x%x -> ratio=%u, rate(before)=%lu\n",
                        cur, GET_TYPE(cur), raw, ratio, rate);

                do_div(rate, ratio);

                pr_info("RA: ra_recalc_rate: rate(after)=%lu\n", rate);
            } else {
                pr_info("RA: ra_recalc_rate: skip node id=0x%x type=0x%x (not "
                        "FIXED_FACTOR/DIV)\n",
                        cur, GET_TYPE(cur));
                continue;
            }
        }

        pr_info("RA: ra_recalc_rate: exit id=0x%x -> rate=%lu\n", id, rate);
    out:
        ra_log_pop(log);
        return rate;
    }
    EXPORT_SYMBOL_GPL(ra_recalc_rate);

    int __init ra_init(void) {
        struct cmucal_clk *clk;
        struct sfr_block *block;
        int i;
        int size;
        struct cmucal_qch *qch;

        /* convert physical address to virtual address */
        size = cmucal_get_list_size(SFR_BLOCK_TYPE);
        for (i = 0; i < size; i++) {
            block = cmucal_get_sfr_node(i | SFR_BLOCK_TYPE);
            if (block && block->pa)
                block->va = ioremap(block->pa, block->size);
        }

        size = cmucal_get_list_size(PLL_TYPE);
        for (i = 0; i < size; i++) {
            clk = cmucal_get_node(i | PLL_TYPE);
            if (!clk)
                continue;

            /* Only allow verbose logging when this PLL belongs to G3D */
            {
                struct ra_log_ctx log = ra_log_push_clk(clk, clk->id);

            ra_get_pll_address(clk);

            ra_get_pll_rate_table(clk);

            pll_get_locktime(to_pll_clk(clk));

                ra_log_pop(log);
            }
        }

        size = cmucal_get_list_size(MUX_TYPE);
        for (i = 0; i < size; i++) {
            clk = cmucal_get_node(i | MUX_TYPE);
            if (!clk)
                continue;

            if (GET_IDX(clk->offset_idx) != EMPTY_CAL_ID)
                clk->paddr = ra_get_sfr_address(clk->offset_idx, &clk->offset,
                                                &clk->shift, &clk->width);
            else
                clk->offset = NULL;

            if (GET_IDX(clk->status_idx) != EMPTY_CAL_ID)
                ra_get_sfr_address(clk->status_idx, &clk->status, &clk->s_shift,
                                   &clk->s_width);
            else
                clk->status = NULL;

            if (GET_IDX(clk->enable_idx) != EMPTY_CAL_ID)
                ra_get_sfr_address(clk->enable_idx, &clk->enable, &clk->e_shift,
                                   &clk->e_width);
            else
                clk->enable = NULL;
        }

        size = cmucal_get_list_size(DIV_TYPE);
        for (i = 0; i < size; i++) {
            clk = cmucal_get_node(i | DIV_TYPE);
            if (!clk)
                continue;

            if (GET_IDX(clk->offset_idx) != EMPTY_CAL_ID)
                clk->paddr = ra_get_sfr_address(clk->offset_idx, &clk->offset,
                                                &clk->shift, &clk->width);
            else
                clk->offset = NULL;

            if (GET_IDX(clk->status_idx) != EMPTY_CAL_ID)
                ra_get_sfr_address(clk->status_idx, &clk->status, &clk->s_shift,
                                   &clk->s_width);
            else
                clk->status = NULL;

            if (GET_IDX(clk->enable_idx) != EMPTY_CAL_ID)
                ra_get_sfr_address(clk->enable_idx, &clk->enable, &clk->e_shift,
                                   &clk->e_width);
            else
                clk->enable = NULL;
        }

        size = cmucal_get_list_size(GATE_TYPE);
        for (i = 0; i < size; i++) {
            clk = cmucal_get_node(i | GATE_TYPE);
            if (!clk)
                continue;

            if (GET_IDX(clk->offset_idx) != EMPTY_CAL_ID)
                clk->paddr = ra_get_sfr_address(clk->offset_idx, &clk->offset,
                                                &clk->shift, &clk->width);
            else
                clk->offset = NULL;

            if (GET_IDX(clk->status_idx) != EMPTY_CAL_ID)
                ra_get_sfr_address(clk->status_idx, &clk->status, &clk->s_shift,
                                   &clk->s_width);
            else
                clk->status = NULL;

            if (GET_IDX(clk->enable_idx) != EMPTY_CAL_ID)
                ra_get_sfr_address(clk->enable_idx, &clk->enable, &clk->e_shift,
                                   &clk->e_width);
            else
                clk->enable = NULL;
        }

        size = cmucal_get_list_size(FIXED_RATE_TYPE);
        for (i = 0; i < size; i++) {
            clk = cmucal_get_node(i | FIXED_RATE_TYPE);
            if (!clk)
                continue;

            if (GET_IDX(clk->enable_idx) != EMPTY_CAL_ID)
                ra_get_sfr_address(clk->enable_idx, &clk->enable, &clk->e_shift,
                                   &clk->e_width);
            else
                clk->enable = NULL;
        }

        size = cmucal_get_list_size(FIXED_FACTOR_TYPE);
        for (i = 0; i < size; i++) {
            clk = cmucal_get_node(i | FIXED_FACTOR_TYPE);
            if (!clk)
                continue;

            if (GET_IDX(clk->enable_idx) != EMPTY_CAL_ID)
                ra_get_sfr_address(clk->enable_idx, &clk->enable, &clk->e_shift,
                                   &clk->e_width);
            else
                clk->enable = NULL;
        }

        size = cmucal_get_list_size(QCH_TYPE);
        for (i = 0; i < size; i++) {
            clk = cmucal_get_node(i | QCH_TYPE);
            if (!clk)
                continue;

            clk->paddr = ra_get_sfr_address(clk->offset_idx, &clk->offset,
                                            &clk->shift, &clk->width);

            if (GET_IDX(clk->status_idx) != EMPTY_CAL_ID)
                ra_get_sfr_address(clk->status_idx, &clk->status, &clk->s_shift,
                                   &clk->s_width);
            else
                clk->status = NULL;

            if (GET_IDX(clk->enable_idx) != EMPTY_CAL_ID)
                ra_get_sfr_address(clk->enable_idx, &clk->enable, &clk->e_shift,
                                   &clk->e_width);
            else
                clk->enable = NULL;

            qch = to_qch(clk);
            if (GET_IDX(qch->ignore_idx) != EMPTY_CAL_ID)
                ra_get_sfr_address(qch->ignore_idx, &qch->ignore,
                                   &qch->ig_shift, &qch->ig_width);
            else
                qch->ignore = NULL;
        }

        size = cmucal_get_list_size(OPTION_TYPE);
        for (i = 0; i < size; i++) {
            clk = cmucal_get_node(i | OPTION_TYPE);
            if (!clk)
                continue;

            ra_get_sfr_address(clk->offset_idx, &clk->offset, &clk->shift,
                               &clk->width);

            if (GET_IDX(clk->enable_idx) != EMPTY_CAL_ID)
                ra_get_sfr_address(clk->enable_idx, &clk->enable, &clk->e_shift,
                                   &clk->e_width);
            else
                clk->enable = NULL;
        }

        return 0;
    }
