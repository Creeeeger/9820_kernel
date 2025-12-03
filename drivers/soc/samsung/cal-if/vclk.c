#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <soc/samsung/ect_parser.h>

#include "acpm_dvfs.h"
#include "asv.h"
#include "cmucal.h"
#include "gpu_dvfs_overrides.h"
#include "ra.h"
#include "vclk.h"
#include <linux/errno.h>
#include <linux/printk.h>

#define ECT_DUMMY_SFR (0xFFFFFFFF)
unsigned int asv_table_ver = 0;
unsigned int main_rev;
unsigned int sub_rev;

static void vclk_debug_dump_lut(struct vclk *vclk) {
    int i, j;

    if (!vclk || !vclk->lut) {
        pr_info("[vclk-debug] no lut for vclk\n");
        return;
    }

    pr_info("[vclk-debug] dump for vclk '%s': num_rates=%d, num_list=%d\n",
            vclk->name, vclk->num_rates, vclk->num_list);

    for (i = 0; i < vclk->num_rates; i++) {
        struct vclk_lut *l = &vclk->lut[i];

        pr_info("[vclk-debug]   lut[%02d] rate=%u\n", i, vclk->lut[i].rate);
        for (j = 0; j < vclk->num_list; j++) {
            pr_info("[vclk-debug]     param[%02d] (clk_id=0x%x) = 0x%x\n", j,
                    vclk->list[j], vclk->lut[i].params[j]);
        }

        pr_cont("\n");
    }
}

static struct vclk_lut *get_lut(struct vclk *vclk, unsigned int rate) {
    int i;

    for (i = 0; i < vclk->num_rates; i++)
        if (rate >= vclk->lut[i].rate)
            break;

    if (i == vclk->num_rates)
        return NULL;

    return &vclk->lut[i];
}

static unsigned int get_max_rate(unsigned int from, unsigned int to) {
    unsigned int max_rate;

    if (from)
        max_rate = (from > to) ? from : to;
    else
        max_rate = to;

    return max_rate;
}

static void __select_switch_pll(struct vclk *vclk, unsigned int rate,
                                unsigned int select) {
    if (vclk->ops && vclk->ops->switch_pre)
        vclk->ops->switch_pre(vclk->vrate, rate);

    if (vclk->ops && vclk->ops->switch_trans && select)
        vclk->ops->switch_trans(vclk->vrate, rate);
    else if (vclk->ops && vclk->ops->restore_trans && !select)
        vclk->ops->restore_trans(vclk->vrate, rate);
    else
        ra_select_switch_pll(vclk->switch_info, select);

    if (vclk->ops && vclk->ops->switch_post)
        vclk->ops->switch_post(vclk->vrate, rate);
}

static int transition_switch(struct vclk *vclk, struct vclk_lut *lut,
                             unsigned int switch_rate) {
    unsigned int *list = vclk->list;
    unsigned int num_list = vclk->num_list;

    /* Change to swithing PLL */
    if (vclk->ops && vclk->ops->trans_pre)
        vclk->ops->trans_pre(vclk->vrate, lut->rate);

    ra_set_clk_by_type(list, lut, num_list, DIV_TYPE, TRANS_HIGH);

    __select_switch_pll(vclk, switch_rate, 1);

    ra_set_clk_by_type(list, lut, num_list, MUX_TYPE, TRANS_FORCE);
    ra_set_clk_by_type(list, lut, num_list, DIV_TYPE, TRANS_LOW);

    vclk->vrate = switch_rate;

    return 0;
}

static int transition_restore(struct vclk *vclk, struct vclk_lut *lut) {
    unsigned int *list = vclk->list;
    unsigned int num_list = vclk->num_list;

    /* PLL setting */
    ra_set_pll_ops(list, lut, num_list, vclk->ops);

    ra_set_clk_by_type(list, lut, num_list, DIV_TYPE, TRANS_HIGH);

    __select_switch_pll(vclk, lut->rate, 0);

    ra_set_clk_by_type(list, lut, num_list, MUX_TYPE, TRANS_FORCE);
    ra_set_clk_by_type(list, lut, num_list, DIV_TYPE, TRANS_LOW);
    if (vclk->ops && vclk->ops->trans_post)
        vclk->ops->trans_post(vclk->vrate, lut->rate);

    return 0;
}

static int transition(struct vclk *vclk, struct vclk_lut *lut) {
    unsigned int *list = vclk->list;
    unsigned int num_list = vclk->num_list;

    ra_set_clk_by_type(list, lut, num_list, DIV_TYPE, TRANS_HIGH);
    ra_set_clk_by_type(list, lut, num_list, PLL_TYPE, TRANS_LOW);
    ra_set_clk_by_type(list, lut, num_list, MUX_TYPE, TRANS_FORCE);
    ra_set_clk_by_type(list, lut, num_list, PLL_TYPE, TRANS_HIGH);
    ra_set_clk_by_type(list, lut, num_list, DIV_TYPE, TRANS_LOW);

    return 0;
}

static bool is_switching_pll_ops(struct vclk *vclk, int cmd) {
    int i;

    if (!vclk->switch_info)
        return false;

    if (cmd != ONESHOT_TRANS)
        return true;

    for (i = 0; i < vclk->num_list; i++) {
        if (IS_PLL(vclk->list[i]))
            return true;
    }

    return false;
}

static int __vclk_set_rate(unsigned int id, unsigned int rate, int cmd) {
    struct vclk *vclk;
    struct vclk_lut *new_lut, *switch_lut;
    unsigned int switch_rate, max_rate;

    pr_info("VCLK: __vclk_set_rate: enter id=%u rate=%u cmd=0x%x IS_VCLK=%d "
            "DFS=%d COMMON=%d\n",
            id, rate, cmd, IS_VCLK(id), IS_DFS_VCLK(id), IS_COMMON_VCLK(id));

    if (!IS_VCLK(id)) {
        pr_info("VCLK: id=%u not a VCLK -> ra_set_rate(id=%u, rate=%u)\n", id,
                id, rate);
        return ra_set_rate(id, rate);
    }

    vclk = cmucal_get_node(id);
    pr_info("VCLK: cmucal_get_node(id=%u) -> vclk=%p\n", id, vclk);

    if (!vclk) {
        pr_info("VCLK: ERROR: vclk is NULL (id=%u) -> -EVCLKINVAL\n", id);
        return -EVCLKINVAL;
    }

    pr_info("VCLK: vclk fields: lut=%p seq=%p list=%p num_list=%u vrate=%u "
            "switch_info=%p\n",
            vclk->lut, vclk->seq, vclk->list, vclk->num_list, vclk->vrate,
            vclk->switch_info);

    if (!vclk->lut) {
        pr_info("VCLK: ERROR: vclk->lut is NULL (id=%u) -> -EVCLKINVAL\n", id);
        return -EVCLKINVAL;
    }

    /* Determine LUT lookup rate unit */
    if (IS_DFS_VCLK(id) || IS_COMMON_VCLK(id)) {
        pr_info("VCLK: LUT lookup using rate=%u (DFS/COMMON)\n", rate);
        new_lut = get_lut(vclk, rate);
    } else {
        pr_info("VCLK: LUT lookup using rate/1000=%u (non-DFS/COMMON), raw "
                "rate=%u\n",
                rate / 1000, rate);
        new_lut = get_lut(vclk, rate / 1000);
    }

    pr_info("VCLK: get_lut(vclk=%p, ...) -> new_lut=%p\n", vclk, new_lut);

    if (!new_lut) {
        pr_info("VCLK: ERROR: new_lut is NULL (id=%u rate=%u) -> -EVCLKINVAL\n",
                id, rate);
        return -EVCLKINVAL;
    }

    if (is_switching_pll_ops(vclk, cmd)) {
        pr_info("VCLK: switching pll ops path (cmd=0x%x)\n", cmd);

        switch_lut = new_lut;
        switch_rate = rate;

        pr_info("VCLK: initial switch_lut=%p switch_rate=%u oneshot=%d "
                "switch=%d restore=%d\n",
                switch_lut, switch_rate, is_oneshot_trans(cmd),
                is_switch_trans(cmd), is_restore_trans(cmd));

        if (is_oneshot_trans(cmd)) {
            max_rate = get_max_rate(vclk->vrate, rate);
            pr_info("VCLK: oneshot: get_max_rate(old_vrate=%u, req_rate=%u) -> "
                    "max_rate=%u\n",
                    vclk->vrate, rate, max_rate);

            switch_rate = ra_set_rate_switch(vclk->switch_info, max_rate);
            pr_info("VCLK: oneshot: ra_set_rate_switch(switch_info=%p, "
                    "max_rate=%u) -> switch_rate=%u\n",
                    vclk->switch_info, max_rate, switch_rate);

            switch_lut = get_lut(vclk, switch_rate);
            pr_info("VCLK: oneshot: get_lut(vclk=%p, switch_rate=%u) -> "
                    "switch_lut=%p\n",
                    vclk, switch_rate, switch_lut);

            if (!switch_lut) {
                pr_info("VCLK: ERROR: switch_lut is NULL (switch_rate=%u) -> "
                        "-EVCLKINVAL\n",
                        switch_rate);
                return -EVCLKINVAL;
            }
        }

        if (is_switch_trans(cmd)) {
            pr_info("VCLK: transition_switch(vclk=%p, switch_lut=%p, "
                    "switch_rate=%u)\n",
                    vclk, switch_lut, switch_rate);
            transition_switch(vclk, switch_lut, switch_rate);
        }

        if (is_restore_trans(cmd)) {
            pr_info("VCLK: transition_restore(vclk=%p, new_lut=%p)\n", vclk,
                    new_lut);
            transition_restore(vclk, new_lut);
        }
    } else if (vclk->seq) {
        pr_info("VCLK: seq path: ra_set_clk_by_seq(list=%p, new_lut=%p, "
                "seq=%p, num_list=%u)\n",
                vclk->list, new_lut, vclk->seq, vclk->num_list);
        ra_set_clk_by_seq(vclk->list, new_lut, vclk->seq, vclk->num_list);
    } else {
        pr_info(
            "VCLK: direct transition path: transition(vclk=%p, new_lut=%p)\n",
            vclk, new_lut);
        transition(vclk, new_lut);
    }

    pr_info("VCLK: updating vclk->vrate old=%u new=%u (vclk=%p)\n", vclk->vrate,
            rate, vclk);
    vclk->vrate = rate;

    pr_info("VCLK: __vclk_set_rate: exit OK id=%u rate=%u cmd=0x%x\n", id, rate,
            cmd);
    return 0;
}

int vclk_set_rate(unsigned int id, unsigned long rate) {
    int ret;

    pr_info("VCLK: vclk_set_rate: enter id=%u rate=%lu\n", id, rate);

    ret = __vclk_set_rate(id, (unsigned int)rate, ONESHOT_TRANS);

    pr_info("VCLK: vclk_set_rate: exit id=%u rate=%lu ret=%d\n", id, rate, ret);
    return ret;
}

int vclk_set_rate_switch(unsigned int id, unsigned long rate) {
    int ret;

    pr_info("VCLK: vclk_set_rate_switch: enter id=%u rate=%lu "
            "cmd=SWITCH_TRANS(0x%x) IS_VCLK=%d\n",
            id, rate, SWITCH_TRANS, IS_VCLK(id));

    ret = __vclk_set_rate(id, (unsigned int)rate, SWITCH_TRANS);

    pr_info("VCLK: vclk_set_rate_switch: exit id=%u rate=%lu ret=%d\n", id,
            rate, ret);
    return ret;
}

int vclk_set_rate_restore(unsigned int id, unsigned long rate) {
    int ret;

    pr_info("VCLK: vclk_set_rate_restore: enter id=%u rate=%lu "
            "cmd=RESTORE_TRANS(0x%x) IS_VCLK=%d\n",
            id, rate, RESTORE_TRANS, IS_VCLK(id));

    ret = __vclk_set_rate(id, (unsigned int)rate, RESTORE_TRANS);

    pr_info("VCLK: vclk_set_rate_restore: exit id=%u rate=%lu ret=%d\n", id,
            rate, ret);
    return ret;
}

unsigned long vclk_recalc_rate(unsigned int id) {
    struct vclk *vclk;
    int i, ret;

    pr_info("VCLK: vclk_recalc_rate: enter id=%u IS_VCLK=%d\n", id,
            IS_VCLK(id));

    if (!IS_VCLK(id)) {
        unsigned long r = ra_recalc_rate(id);
        pr_info("VCLK: vclk_recalc_rate: id=%u not VCLK -> "
                "ra_recalc_rate(%u)=%lu\n",
                id, id, r);
        return r;
    }

    vclk = cmucal_get_node(id);
    pr_info("VCLK: vclk_recalc_rate: cmucal_get_node(%u) -> vclk=%p\n", id,
            vclk);
    if (!vclk) {
        pr_info("VCLK: vclk_recalc_rate: ERROR vclk NULL (id=%u) -> 0\n", id);
        return 0;
    }

    pr_info("VCLK: vclk_recalc_rate: vclk fields: vclk->id=%u vrate(old)=%u "
            "lut=%p num_rates=%u list=%p num_list=%u\n",
            vclk->id, vclk->vrate, vclk->lut, vclk->num_rates, vclk->list,
            vclk->num_list);

    if (IS_DFS_VCLK(vclk->id) || IS_COMMON_VCLK(vclk->id) ||
        IS_ACPM_VCLK(vclk->id)) {
        pr_info("VCLK: vclk_recalc_rate: DFS/COMMON/ACPM path (DFS=%d "
                "COMMON=%d ACPM=%d)\n",
                IS_DFS_VCLK(vclk->id), IS_COMMON_VCLK(vclk->id),
                IS_ACPM_VCLK(vclk->id));

        if (!vclk->lut) {
            pr_info("VCLK: vclk_recalc_rate: ERROR vclk->lut NULL (id=%u) -> "
                    "vrate=0\n",
                    id);
            vclk->vrate = 0;
            return 0;
        }

        for (i = 0; i < vclk->num_rates; i++) {
            pr_info("VCLK: compare: i=%d lut[i]=%p lut_rate=%u lut_params=%p "
                    "list=%p num_list=%u\n",
                    i, &vclk->lut[i], vclk->lut[i].rate, vclk->lut[i].params,
                    vclk->list, vclk->num_list);

            ret = ra_compare_clk_list(vclk->lut[i].params, vclk->list,
                                      vclk->num_list);

            pr_info("VCLK: ra_compare_clk_list -> ret=%d (0 means match)\n",
                    ret);

            if (!ret) {
                pr_info("VCLK: MATCH: i=%d -> set vrate=%u\n", i,
                        vclk->lut[i].rate);
                vclk->vrate = vclk->lut[i].rate;
                break;
            }
        }

        if (i == vclk->num_rates) {
            vclk->vrate = 0;
            pr_info("VCLK: vclk_recalc_rate: FAILED to match any LUT entry: "
                    "id=%u num_rates=%u -> vrate=0\n",
                    id, vclk->num_rates);
        }
    } else {
        unsigned long r;

        pr_info("VCLK: vclk_recalc_rate: non-DFS/COMMON/ACPM path -> "
                "ra_recalc_rate(list[0])\n");

        if (!vclk->list) {
            pr_info("VCLK: vclk_recalc_rate: ERROR vclk->list NULL (id=%u) -> "
                    "vrate=0\n",
                    id);
            vclk->vrate = 0;
            return 0;
        }

        pr_info("VCLK: vclk_recalc_rate: list[0]=%u\n", vclk->list[0]);

        r = ra_recalc_rate(vclk->list[0]);

        pr_info(
            "VCLK: vclk_recalc_rate: ra_recalc_rate(%u)=%lu -> set vrate=%lu\n",
            vclk->list[0], r, r);

        vclk->vrate = r;
    }

    pr_info("VCLK: vclk_recalc_rate: exit id=%u vrate(new)=%u\n", id,
            vclk->vrate);
    return vclk->vrate;
}

unsigned long vclk_get_rate(unsigned int id) {
    struct vclk *vclk;

    pr_info("VCLK: vclk_get_rate: enter id=%u IS_VCLK=%d\n", id, IS_VCLK(id));

    if (IS_VCLK(id)) {
        vclk = cmucal_get_node(id);
        pr_info("VCLK: vclk_get_rate: cmucal_get_node(%u) -> vclk=%p\n", id,
                vclk);
        if (vclk) {
            pr_info(
                "VCLK: vclk_get_rate: return vclk->vrate=%u (vclk->id=%u)\n",
                vclk->vrate, vclk->id);
            return vclk->vrate;
        }
        pr_info("VCLK: vclk_get_rate: vclk NULL -> return 0\n");
    } else {
        pr_info("VCLK: vclk_get_rate: id=%u not VCLK -> return 0\n", id);
    }

    return 0;
}

int vclk_set_enable(unsigned int id) {
    struct vclk *vclk;
    int ret = -EVCLKINVAL;

    if (IS_GATE_VCLK(id)) {
        vclk = cmucal_get_node(id);
        if (vclk)
            ret = ra_set_list_enable(vclk->list, vclk->num_list);
    } else if (IS_VCLK(id)) {
        ret = 0;
    } else {
        ret = ra_set_enable(id, 1);
    }

    return ret;
}

int vclk_set_disable(unsigned int id) {
    struct vclk *vclk;
    int ret = -EVCLKINVAL;

    if (IS_GATE_VCLK(id)) {
        vclk = cmucal_get_node(id);
        if (vclk)
            ret = ra_set_list_disable(vclk->list, vclk->num_list);
    } else if (IS_VCLK(id)) {
        ret = 0;
    } else {
        ret = ra_set_enable(id, 0);
    }

    return ret;
}

unsigned int vclk_get_lv_num(unsigned int id) {
    struct vclk *vclk;
    int lv_num = 0;

    pr_info("VCLK: vclk_get_lv_num: enter id=%u\n", id);

    vclk = cmucal_get_node(id);
    pr_info("VCLK: vclk_get_lv_num: cmucal_get_node(%u) -> vclk=%p\n", id,
            vclk);

    if (vclk)
        pr_info("VCLK: vclk_get_lv_num: vclk->id=%u lut=%p num_rates=%u\n",
                vclk->id, vclk->lut, vclk->num_rates);

    if (vclk && vclk->lut)
        lv_num = vclk->num_rates;

    pr_info("VCLK: vclk_get_lv_num: exit id=%u lv_num=%d\n", id, lv_num);
    return lv_num;
}

unsigned int vclk_get_max_freq(unsigned int id) {
    struct vclk *vclk;
    int rate = 0;

    pr_info("VCLK: vclk_get_max_freq: enter id=%u\n", id);

    vclk = cmucal_get_node(id);
    pr_info("VCLK: vclk_get_max_freq: cmucal_get_node(%u) -> vclk=%p\n", id,
            vclk);

    if (vclk)
        pr_info("VCLK: vclk_get_max_freq: vclk->id=%u lut=%p max_freq=%u\n",
                vclk->id, vclk->lut, vclk->max_freq);

    if (vclk && vclk->lut)
        rate = vclk->max_freq;

    pr_info("VCLK: vclk_get_max_freq: exit id=%u max_freq=%d\n", id, rate);
    return rate;
}

unsigned int vclk_get_min_freq(unsigned int id) {
    struct vclk *vclk;
    int rate = 0;

    pr_info("VCLK: vclk_get_min_freq: enter id=%u\n", id);

    vclk = cmucal_get_node(id);
    pr_info("VCLK: vclk_get_min_freq: cmucal_get_node(%u) -> vclk=%p\n", id,
            vclk);

    if (vclk)
        pr_info("VCLK: vclk_get_min_freq: vclk->id=%u lut=%p min_freq=%u\n",
                vclk->id, vclk->lut, vclk->min_freq);

    if (vclk && vclk->lut)
        rate = vclk->min_freq;

    pr_info("VCLK: vclk_get_min_freq: exit id=%u min_freq=%d\n", id, rate);
    return rate;
}

int vclk_get_rate_table(unsigned int id, unsigned long *table) {
    struct vclk *vclk;
    int i;
    unsigned int nums = 0;

    pr_info("VCLK: vclk_get_rate_table: enter id=%u table=%p\n", id, table);

    vclk = cmucal_get_node(id);
    pr_info("VCLK: vclk_get_rate_table: cmucal_get_node(%u) -> vclk=%p\n", id,
            vclk);

    if (!vclk) {
        pr_info("VCLK: vclk_get_rate_table: vclk NULL -> return 0\n");
        return 0;
    }

    pr_info("VCLK: vclk_get_rate_table: vclk->id=%u IS_VCLK(vclk->id)=%d "
            "lut=%p num_rates=%u\n",
            vclk->id, IS_VCLK(vclk->id), vclk->lut, vclk->num_rates);

    if (!IS_VCLK(vclk->id)) {
        pr_info("VCLK: vclk_get_rate_table: vclk->id not VCLK -> return 0\n");
        return 0;
    }

    if (!table) {
        pr_info(
            "VCLK: vclk_get_rate_table: table NULL (caller bug) -> return 0\n");
        return 0;
    }

    if (vclk->lut) {
        for (i = 0; i < vclk->num_rates; i++) {
            table[i] = vclk->lut[i].rate;
            pr_info("VCLK: vclk_get_rate_table: table[%d]=%lu (lut[%d].rate=%u "
                    "lut_entry=%p)\n",
                    i, table[i], i, vclk->lut[i].rate, &vclk->lut[i]);
        }
        nums = vclk->num_rates;
    } else {
        pr_info("VCLK: vclk_get_rate_table: lut NULL -> nums=0\n");
    }

    pr_info("VCLK: vclk_get_rate_table: exit id=%u nums=%u\n", id, nums);
    return nums;
}

int vclk_get_bigturbo_table(unsigned int *table) {
    void *gen_block;
    struct ect_gen_param_table *bigturbo;
    int idx;
    int i;

    gen_block = ect_get_block("GEN");
    if (gen_block == NULL)
        return -EVCLKINVAL;

    bigturbo = ect_gen_param_get_table(gen_block, "BIGTURBO");
    if (bigturbo == NULL)
        return -EVCLKINVAL;

    if (bigturbo->num_of_row == 0)
        return -EVCLKINVAL;

    if (asv_table_ver >= bigturbo->num_of_row)
        idx = bigturbo->num_of_row - 1;
    else
        idx = asv_table_ver;

    for (i = 0; i < bigturbo->num_of_col; i++)
        table[i] = bigturbo->parameter[idx * bigturbo->num_of_col + i];

    return 0;
}

unsigned int vclk_get_boot_freq(unsigned int id) {
    struct vclk *vclk;
    unsigned int rate = 0;

    vclk = cmucal_get_node(id);
    if (!vclk || !(IS_DFS_VCLK(vclk->id) || IS_ACPM_VCLK(vclk->id)))
        return rate;

    if (vclk->boot_freq)
        rate = vclk->boot_freq;
    else
        rate = (unsigned int)vclk_recalc_rate(id);

    return rate;
}

unsigned int vclk_get_resume_freq(unsigned int id) {
    struct vclk *vclk;
    unsigned int rate = 0;

    vclk = cmucal_get_node(id);
    if (!vclk || !(IS_DFS_VCLK(vclk->id) || IS_ACPM_VCLK(vclk->id)))
        return rate;

    if (vclk->resume_freq)
        rate = vclk->resume_freq;
    else
        rate = (unsigned int)vclk_recalc_rate(id);

    return rate;
}

static int vclk_get_dfs_info(struct vclk *vclk) {
    int i, j, k;
    void *dvfs_block;
    struct ect_dvfs_domain *dvfs_domain;
    void *gen_block;
    struct ect_gen_param_table *minmax = NULL;
    unsigned int *minmax_table = NULL;
    int *params, idx;
    int original_num_rates;
    int alloc_num_rates;
    unsigned int original_max_rate = 0;
    unsigned long highest_override = 0;
    size_t override_count = 0;
    bool is_gpu = false;
    bool descending = false;
    int current_num_rates;
    int ret = 0;
    char buf[32];

    pr_info("[vclk][dfs] enter vclk=%p name=%s\n", vclk,
            vclk ? vclk->name : "(null)");
    if (!vclk || !vclk->name) {
        pr_info("[vclk][dfs] ERROR: invalid vclk/name\n");
        return -EVCLKINVAL;
    }

    dvfs_block = ect_get_block("DVFS");
    pr_info("[vclk][dfs] ect_get_block(\"DVFS\") -> %p\n", dvfs_block);
    if (dvfs_block == NULL)
        return -EVCLKNOENT;

    dvfs_domain = ect_dvfs_get_domain(dvfs_block, vclk->name);
    pr_info("[vclk][dfs] ect_dvfs_get_domain(DVFS, \"%s\") -> %p\n", vclk->name,
            dvfs_domain);
    if (dvfs_domain == NULL)
        return -EVCLKINVAL;

    pr_info("[vclk][dfs] dvfs_domain summary: num_of_level=%d num_of_clock=%d "
            "max_frequency=%u min_frequency=%u boot_level_idx=%d "
            "resume_level_idx=%d\n",
            dvfs_domain->num_of_level, dvfs_domain->num_of_clock,
            dvfs_domain->max_frequency, dvfs_domain->min_frequency,
            dvfs_domain->boot_level_idx, dvfs_domain->resume_level_idx);

    pr_info("[vclk][dfs] dvfs_domain ptrs: list_level=%p list_dvfs_value=%p\n",
            dvfs_domain->list_level, dvfs_domain->list_dvfs_value);

    /* GEN/MINMAX lookup */
    gen_block = ect_get_block("GEN");
    pr_info("[vclk][dfs] ect_get_block(\"GEN\") -> %p\n", gen_block);

    if (gen_block) {
        snprintf(buf, sizeof(buf), "MINMAX_%s", vclk->name);
        pr_info("[vclk][dfs] looking for GEN table \"%s\"\n", buf);

        minmax = ect_gen_param_get_table(gen_block, buf);
        pr_info("[vclk][dfs] ect_gen_param_get_table(GEN, \"%s\") -> %p\n", buf,
                minmax);

        if (minmax != NULL) {
            pr_info("[vclk][dfs] minmax table meta: rows=%d cols=%d "
                    "parameter=%p asv_table_ver=%u\n",
                    minmax->num_of_row, minmax->num_of_col, minmax->parameter,
                    asv_table_ver);

            for (i = 0; i < minmax->num_of_row; i++) {
                minmax_table = &minmax->parameter[minmax->num_of_col * i];
                pr_info(
                    "[vclk][dfs] minmax row=%d table_ptr=%p ver(field0)=%u\n",
                    i, minmax_table, minmax_table ? minmax_table[0] : 0);

                if (minmax_table && minmax_table[0] == asv_table_ver) {
                    pr_info("[vclk][dfs] minmax match: row=%d\n", i);
                    break;
                }
            }
        }
    }

    /* Populate vclk core fields from DVFS domain */
    vclk->num_rates = dvfs_domain->num_of_level;
    vclk->num_list = dvfs_domain->num_of_clock;
    vclk->max_freq = dvfs_domain->max_frequency;
    vclk->min_freq = dvfs_domain->min_frequency;

    original_num_rates = vclk->num_rates;
    alloc_num_rates = original_num_rates;
    is_gpu = !strcmp(vclk->name, "dvfs_g3d");

    pr_info("[vclk][dfs] vclk seeded: num_rates=%d num_list=%d min_freq=%u "
            "max_freq=%u is_gpu=%d\n",
            vclk->num_rates, vclk->num_list, vclk->min_freq, vclk->max_freq,
            is_gpu);

    if (is_gpu && gpu_dvfs_has_overrides()) {
        override_count = gpu_dvfs_override_count();
        pr_info("[vclk][dfs] GPU overrides: has_overrides=1 count=%zu\n",
                override_count);
        if (override_count)
            alloc_num_rates += override_count;
    } else if (is_gpu) {
        pr_info("[vclk][dfs] GPU overrides: has_overrides=0\n");
    }

    if (minmax_table != NULL) {
        pr_info("[vclk][dfs] applying MINMAX override: raw_min=%u raw_max=%u "
                "(kHz?) -> x1000\n",
                minmax_table[MINMAX_MIN_FREQ], minmax_table[MINMAX_MAX_FREQ]);

        vclk->min_freq = minmax_table[MINMAX_MIN_FREQ] * 1000;
        vclk->max_freq = minmax_table[MINMAX_MAX_FREQ] * 1000;
    } else {
        pr_info("[vclk][dfs] MINMAX table absent for %s\n", vclk->name);
    }

    pr_info("ACPM_DVFS :%s\n", vclk->name);

    /* Allocate list + lut */
    pr_info("[vclk][dfs] alloc: list bytes=%zu (num_list=%d)\n",
            sizeof(unsigned int) * (size_t)vclk->num_list, vclk->num_list);
    vclk->list = kzalloc(sizeof(unsigned int) * vclk->num_list, GFP_KERNEL);
    pr_info("[vclk][dfs] kzalloc list -> %p\n", vclk->list);
    if (!vclk->list)
        return -EVCLKNOMEM;

    pr_info("[vclk][dfs] alloc: lut bytes=%zu (alloc_num_rates=%d)\n",
            sizeof(struct vclk_lut) * (size_t)alloc_num_rates, alloc_num_rates);
    vclk->lut = kzalloc(sizeof(struct vclk_lut) * alloc_num_rates, GFP_KERNEL);
    pr_info("[vclk][dfs] kzalloc lut -> %p\n", vclk->lut);
    if (!vclk->lut) {
        ret = -EVCLKNOMEM;
        goto err_nomem1;
    }

    /* Fill lut[] from DVFS domain */
    pr_info("[vclk][dfs] filling LUT: original_num_rates=%d num_list=%d\n",
            original_num_rates, vclk->num_list);

    for (i = 0; i < original_num_rates; i++) {
        vclk->lut[i].rate = dvfs_domain->list_level[i].level;

        pr_info("[vclk][dfs] lut[%d] rate=%u list_level[%d]=%p\n", i,
                vclk->lut[i].rate, i, &dvfs_domain->list_level[i]);

        params = kcalloc(vclk->num_list, sizeof(int), GFP_KERNEL);
        pr_info("[vclk][dfs] kcalloc params for lut[%d] -> %p (bytes=%zu)\n", i,
                params, sizeof(int) * (size_t)vclk->num_list);

        if (!params) {
            ret = -EVCLKNOMEM;
            if (i == 0)
                goto err_nomem2;

            pr_info("[vclk][dfs] ERROR: params alloc failed at i=%d; freeing "
                    "earlier params\n",
                    i);
            for (i = i - 1; i >= 0; i--)
                kfree(vclk->lut[i].params);
            goto err_nomem2;
        }

        for (j = 0; j < vclk->num_list; ++j) {
            idx = i * vclk->num_list + j;
            params[j] = dvfs_domain->list_dvfs_value[idx];
        }
        vclk->lut[i].params = params;

        /* Dump first few params to verify stride/indexing */
        if (vclk->num_list > 0) {
            int dump_n = min(vclk->num_list, 1000);
            pr_info("[vclk][dfs] lut[%d] params[0..%d] =", i, dump_n - 1);
            for (j = 0; j < dump_n; j++)
                pr_cont(" %d", params[j]);
            pr_cont("\n");
        }
    }

    vclk->boot_freq = 0;
    vclk->resume_freq = 0;

    /* Boot/resume freq selection */
    if (minmax_table != NULL) {
        unsigned int want_boot = minmax_table[MINMAX_BOOT_FREQ] * 1000;
        unsigned int want_resume = minmax_table[MINMAX_RESUME_FREQ] * 1000;

        pr_info("[vclk][dfs] boot/resume from MINMAX: want_boot=%u "
                "want_resume=%u\n",
                want_boot, want_resume);

        for (i = 0; i < vclk->num_rates; i++)
            if (vclk->lut[i].rate == want_boot)
                vclk->boot_freq = vclk->lut[i].rate;

        for (i = 0; i < vclk->num_rates; i++)
            if (vclk->lut[i].rate == want_resume)
                vclk->resume_freq = vclk->lut[i].rate;
    } else {
        pr_info("[vclk][dfs] boot/resume from DVFS domain idx: boot_idx=%d "
                "resume_idx=%d\n",
                dvfs_domain->boot_level_idx, dvfs_domain->resume_level_idx);

        if (dvfs_domain->boot_level_idx != -1)
            vclk->boot_freq = vclk->lut[dvfs_domain->boot_level_idx].rate;

        if (dvfs_domain->resume_level_idx != -1)
            vclk->resume_freq = vclk->lut[dvfs_domain->resume_level_idx].rate;
    }

    current_num_rates = original_num_rates;

    /* Determine original max rate and sort direction */
    for (i = 0; i < original_num_rates; i++)
        if (vclk->lut[i].rate > original_max_rate)
            original_max_rate = vclk->lut[i].rate;

    if (original_num_rates >= 2)
        descending = vclk->lut[1].rate < vclk->lut[0].rate;

    pr_info("[vclk][dfs] rate stats: original_max_rate=%u descending=%d\n",
            original_max_rate, descending);

    /* GPU override insertion */
    if (is_gpu && override_count) {
        size_t override_idx;

        pr_info("[vclk][dfs] applying %zu GPU overrides (alloc_num_rates=%d)\n",
                override_count, alloc_num_rates);

        for (override_idx = 0; override_idx < override_count; override_idx++) {
            const struct gpu_dvfs_override_entry *entry;
            unsigned int *override_params;
            int insert_idx = current_num_rates;
            int template_idx;
            bool found = false;

            entry = gpu_dvfs_override_get(override_idx);
            pr_info("[vclk][dfs] override[%zu] gpu_dvfs_override_get -> %p\n",
                    override_idx, entry);
            if (!entry)
                continue;

            pr_info("[vclk][dfs] override[%zu] rate_khz=%lu\n", override_idx,
                    entry->rate_khz);

            highest_override = max(highest_override, entry->rate_khz);

            /* Find duplicate or insertion position */
            for (i = 0; i < current_num_rates; i++) {
                if (vclk->lut[i].rate == entry->rate_khz) {
                    found = true;
                    pr_info("[vclk][dfs] override[%zu] rate already present at "
                            "lut[%d]\n",
                            override_idx, i);
                    break;
                }

                if (descending) {
                    if (entry->rate_khz > vclk->lut[i].rate &&
                        insert_idx == current_num_rates)
                        insert_idx = i;
                } else {
                    if (entry->rate_khz < vclk->lut[i].rate &&
                        insert_idx == current_num_rates)
                        insert_idx = i;
                }
            }

            pr_info("[vclk][dfs] override[%zu] found=%d computed insert_idx=%d "
                    "current_num_rates=%d\n",
                    override_idx, found, insert_idx, current_num_rates);

            if (found)
                continue;

            if (insert_idx > current_num_rates)
                insert_idx = current_num_rates;

            if (!current_num_rates) {
                pr_info("[vclk][dfs] ERROR: current_num_rates=0 during "
                        "override insert\n");
                ret = -EVCLKNOMEM;
                goto err_nomem_override;
            }

            template_idx = (insert_idx < current_num_rates)
                               ? insert_idx
                               : current_num_rates - 1;

            pr_info("[vclk][dfs] override[%zu] template_idx=%d (copy params "
                    "from lut[%d])\n",
                    override_idx, template_idx, template_idx);

            override_params = kcalloc(vclk->num_list, sizeof(int), GFP_KERNEL);
            pr_info("[vclk][dfs] override[%zu] kcalloc override_params -> %p\n",
                    override_idx, override_params);
            if (!override_params) {
                ret = -EVCLKNOMEM;
                goto err_nomem_override;
            }

            memcpy(override_params, vclk->lut[template_idx].params,
                   sizeof(int) * (size_t)vclk->num_list);

            /* Patch PLL params */
            for (k = 0; k < vclk->num_list; k++) {
                if (IS_PLL(vclk->list[k])) {
                    pr_info("[vclk][dfs] override[%zu] patch PLL at "
                            "list[%d]=%u: %d -> %lu\n",
                            override_idx, k, vclk->list[k], override_params[k],
                            entry->rate_khz);
                    override_params[k] = entry->rate_khz;
                }
            }

            /* Shift and insert */
            pr_info("[vclk][dfs] override[%zu] shifting LUT for insertion: "
                    "from=%d down to insert_idx=%d\n",
                    override_idx, current_num_rates, insert_idx);
            for (k = current_num_rates; k > insert_idx; k--)
                vclk->lut[k] = vclk->lut[k - 1];

            vclk->lut[insert_idx].rate = entry->rate_khz;
            vclk->lut[insert_idx].params = override_params;
            current_num_rates++;

            pr_info("[vclk][dfs] override[%zu] inserted at lut[%d], new "
                    "current_num_rates=%d\n",
                    override_idx, insert_idx, current_num_rates);
        }

        vclk->num_rates = current_num_rates;

        pr_info("[vclk][dfs] overrides done: highest_override=%lu "
                "original_max_rate=%u new_num_rates=%d\n",
                highest_override, original_max_rate, vclk->num_rates);

        if (highest_override && vclk->max_freq < highest_override) {
            pr_info("[vclk][dfs] bump max_freq: %u -> %lu\n", vclk->max_freq,
                    highest_override);
            vclk->max_freq = highest_override;
        }

        if (highest_override && vclk->boot_freq == original_max_rate) {
            pr_info("[vclk][dfs] bump boot_freq: %u -> %lu\n", vclk->boot_freq,
                    highest_override);
            vclk->boot_freq = highest_override;
        }

        if (highest_override && vclk->resume_freq == original_max_rate) {
            pr_info("[vclk][dfs] bump resume_freq: %u -> %lu\n",
                    vclk->resume_freq, highest_override);
            vclk->resume_freq = highest_override;
        }

        if (vclk->min_freq > vclk->max_freq) {
            pr_info("[vclk][dfs] clamp min_freq: %u -> %u\n", vclk->min_freq,
                    vclk->max_freq);
            vclk->min_freq = vclk->max_freq;
        }

        if (highest_override)
            pr_info("[vclk] dvfs_g3d max frequency overridden to %lu KHz (was "
                    "%u)\n",
                    highest_override, original_max_rate);
    } else {
        vclk->num_rates = original_num_rates;
        pr_info("[vclk][dfs] no override path: num_rates=%d\n",
                vclk->num_rates);
    }

    pr_info("[vclk] %s domain: levels=%d clocks=%d min=%u max=%u boot=%u "
            "resume=%u (minmax=%s)\n",
            vclk->name, vclk->num_rates, vclk->num_list, vclk->min_freq,
            vclk->max_freq, vclk->boot_freq, vclk->resume_freq,
            minmax_table ? "override" : "absent");

    if (!strcmp(vclk->name, "dvfs_g3d")) {
        pr_info("[vclk] dvfs_g3d boot_idx=%d resume_idx=%d table_ver=%u\n",
                dvfs_domain->boot_level_idx, dvfs_domain->resume_level_idx,
                asv_table_ver);
        for (i = 0; i < vclk->num_rates; i++)
            pr_info("[vclk]   g3d lut[%02d] rate=%u params=%p\n", i,
                    vclk->lut[i].rate, vclk->lut[i].params);
    }

    pr_info("[vclk][dfs] calling vclk_debug_dump_lut(vclk=%p name=%s)\n", vclk,
            vclk->name);
    vclk_debug_dump_lut(vclk);

    pr_info("[vclk][dfs] exit OK ret=%d name=%s\n", ret, vclk->name);
    return ret;

err_nomem_override:
    pr_info("[vclk][dfs] cleanup: err_nomem_override current_num_rates=%d\n",
            current_num_rates);
    while (current_num_rates-- > 0) {
        pr_info("[vclk][dfs] cleanup: kfree lut[%d].params=%p\n",
                current_num_rates, vclk->lut[current_num_rates].params);
        kfree(vclk->lut[current_num_rates].params);
    }
err_nomem2:
    pr_info("[vclk][dfs] cleanup: kfree lut=%p\n", vclk->lut);
    kfree(vclk->lut);
err_nomem1:
    pr_info("[vclk][dfs] cleanup: kfree list=%p\n", vclk->list);
    kfree(vclk->list);

    pr_info("[vclk][dfs] exit ERROR ret=%d name=%s\n", ret, vclk->name);
    return ret;
}

static struct ect_voltage_table *
get_max_min_freq_lv(struct ect_voltage_domain *domain, unsigned int version,
                    int *max_lv, int *min_lv) {
    int i;
    unsigned int max_asv_version = 0;
    struct ect_voltage_table *table = NULL;

    pr_info("ASV: %s: enter domain=%p version=%u max_lv=%p min_lv=%p\n",
            __func__, domain, version, max_lv, min_lv);

    if (!domain || !max_lv || !min_lv) {
        pr_info(
            "ASV: %s: ERROR bad args domain=%p max_lv=%p min_lv=%p -> NULL\n",
            __func__, domain, max_lv, min_lv);
        if (max_lv)
            *max_lv = -1;
        if (min_lv)
            *min_lv = -1;
        return NULL;
    }

    pr_info("ASV: %s: domain fields: num_of_table=%u table_list=%p "
            "num_of_level=%u level_list=%p\n",
            __func__, domain->num_of_table, domain->table_list,
            domain->num_of_level, domain->level_list);

    /* Search requested version, track highest available version */
    for (i = 0; i < domain->num_of_table; i++) {
        table = &domain->table_list[i];

        pr_info("ASV: %s: scan table[%d]=%p table_version=%u (want=%u)\n",
                __func__, i, table, table->table_version, version);

        if (version == table->table_version) {
            pr_info("ASV: %s: FOUND matching table at index=%d version=%u\n",
                    __func__, i, table->table_version);
            break;
        }

        if (table->table_version > max_asv_version) {
            max_asv_version = table->table_version;
            pr_info("ASV: %s: update max_asv_version=%u (from table[%d])\n",
                    __func__, max_asv_version, i);
        }
    }

    if (i == domain->num_of_table) {
        /* NOTE: table currently points to &table_list[last] due to loop, not a
         * match */
        pr_err("ASV: %s: no matching voltage table: requested=%u "
               "current_asv_table_ver=%u max_available=%u -> forcing "
               "asv_table_ver=%u\n",
               __func__, version, asv_table_ver, max_asv_version,
               max_asv_version);
        asv_table_ver = max_asv_version;
        /* keep going: caller may re-call with updated asv_table_ver */
    }

    pr_info("ASV: %s: after scan: i=%d table=%p (domain->num_of_table=%u)\n",
            __func__, i, table, domain->num_of_table);

    if (!table) {
        pr_info(
            "ASV: %s: ERROR table NULL -> *max_lv=*min_lv=-1 and return NULL\n",
            __func__);
        *max_lv = -1;
        *min_lv = -1;
        return NULL;
    }

    *max_lv = -1;
    *min_lv = (int)domain->num_of_level - 1;

    pr_info(
        "ASV: %s: init levels: num_of_level=%u -> init max_lv=%d min_lv=%d\n",
        __func__, domain->num_of_level, *max_lv, *min_lv);

    for (i = 0; i < domain->num_of_level; i++) {
        /* level_en is commonly a per-level enable bitmap/array */
        pr_info("ASV: %s: level scan i=%d level_en=%d (max_lv=%d min_lv=%d)\n",
                __func__, i, table->level_en[i], *max_lv, *min_lv);

        if (*max_lv == -1 && table->level_en[i]) {
            *max_lv = i;
            pr_info("ASV: %s: set max_lv=%d (first enabled level)\n", __func__,
                    *max_lv);
        }

        if (*max_lv != -1 && !table->level_en[i]) {
            *min_lv = i - 1;
            pr_info("ASV: %s: disable boundary at i=%d -> set min_lv=%d and "
                    "break\n",
                    __func__, i, *min_lv);
            break;
        }
    }

    pr_info("ASV: %s: exit table=%p table_version=%u -> max_lv=%d min_lv=%d\n",
            __func__, table, table->table_version, *max_lv, *min_lv);

    return table;
}

static int vclk_get_asv_info(struct vclk *vclk) {
    void *asv_block;
    struct ect_voltage_domain *domain;
    struct ect_voltage_table *table = NULL;
    int max_lv, min_lv;
    int ret = 0;
    char buf[32];
    void *gen_block;
    struct ect_gen_param_table *minmax = NULL;

    pr_info("VCLK: %s: enter vclk=%p\n", __func__, vclk);
    if (!vclk) {
        pr_info("VCLK: %s: ERROR vclk NULL -> -EVCLKINVAL\n", __func__);
        return -EVCLKINVAL;
    }

    pr_info("VCLK: %s: vclk fields: name=%s id=%u num_rates=%u num_list=%u "
            "lut=%p list=%p (old)max=%d min=%d boot=%d resume=%d\n",
            __func__, vclk->name ? vclk->name : "(null)", vclk->id,
            vclk->num_rates, vclk->num_list, vclk->lut, vclk->list,
            vclk->max_freq, vclk->min_freq, vclk->boot_freq, vclk->resume_freq);

    asv_block = ect_get_block("ASV");
    pr_info("VCLK: %s: ect_get_block(\"ASV\") -> %p\n", __func__, asv_block);
    if (asv_block == NULL) {
        pr_info("VCLK: %s: ERROR ASV block missing -> -EVCLKNOENT\n", __func__);
        return -EVCLKNOENT;
    }

    pr_info("VCLK: %s: ect_asv_get_domain(asv_block=%p, name=%s)\n", __func__,
            asv_block, vclk->name ? vclk->name : "(null)");
    domain = ect_asv_get_domain(asv_block, vclk->name);
    pr_info("VCLK: %s: ect_asv_get_domain -> domain=%p\n", __func__, domain);
    if (domain == NULL) {
        pr_info("VCLK: %s: ERROR domain NULL for name=%s -> -EVCLKINVAL\n",
                __func__, vclk->name ? vclk->name : "(null)");
        return -EVCLKINVAL;
    }

    /* Domain-level visibility (pointers + key fields if present) */
    pr_info("VCLK: %s: domain info: level_list=%p (expect kHz entries) "
            "num_levels? (unknown here)\n",
            __func__, domain->level_list);

    gen_block = ect_get_block("GEN");
    pr_info("VCLK: %s: ect_get_block(\"GEN\") -> %p\n", __func__, gen_block);

    if (gen_block) {
        snprintf(buf, sizeof(buf), "MINMAX_%s",
                 vclk->name ? vclk->name : "(null)");
        pr_info("VCLK: %s: lookup minmax table: key=%s\n", __func__, buf);

        minmax = ect_gen_param_get_table(gen_block, buf);
        pr_info("VCLK: %s: ect_gen_param_get_table(gen_block=%p, key=%s) -> "
                "minmax=%p\n",
                __func__, gen_block, buf, minmax);

        if (minmax != NULL) {
            pr_info("VCLK: %s: minmax table present -> skipping "
                    "get_max_min_freq_lv()\n",
                    __func__);
            goto minmax_skip;
        }
    } else {
        pr_info("VCLK: %s: GEN block missing -> using ASV table path\n",
                __func__);
    }

    pr_info("VCLK: %s: get_max_min_freq_lv(domain=%p, asv_table_ver=%d, "
            "&max_lv, &min_lv)\n",
            __func__, domain, asv_table_ver);
    table = get_max_min_freq_lv(domain, asv_table_ver, &max_lv, &min_lv);
    pr_info("VCLK: %s: get_max_min_freq_lv -> table=%p max_lv=%d min_lv=%d\n",
            __func__, table, max_lv, min_lv);

    if (table == NULL) {
        pr_info("VCLK: %s: ERROR table NULL -> -EVCLKFAULT\n", __func__);
        return -EVCLKFAULT;
    }

    /* Compute and log derived freqs (kHz -> Hz via *1000) */
    if (max_lv >= 0) {
        pr_info(
            "VCLK: %s: max_lv=%d level_list[max_lv]=%d (kHz?) -> max_freq=%d\n",
            __func__, max_lv, domain->level_list[max_lv],
            domain->level_list[max_lv] * 1000);
        vclk->max_freq = domain->level_list[max_lv] * 1000;
    } else {
        pr_info("VCLK: %s: max_lv=%d -> max_freq=-1\n", __func__, max_lv);
        vclk->max_freq = -1;
    }

    if (min_lv >= 0) {
        pr_info(
            "VCLK: %s: min_lv=%d level_list[min_lv]=%d (kHz?) -> min_freq=%d\n",
            __func__, min_lv, domain->level_list[min_lv],
            domain->level_list[min_lv] * 1000);
        vclk->min_freq = domain->level_list[min_lv] * 1000;
    } else {
        pr_info("VCLK: %s: min_lv=%d -> min_freq=-1\n", __func__, min_lv);
        vclk->min_freq = -1;
    }

    pr_info("VCLK: %s: table indices: boot_level_idx=%d resume_level_idx=%d\n",
            __func__, table->boot_level_idx, table->resume_level_idx);

    if (table->boot_level_idx >= 0) {
        pr_info("VCLK: %s: boot idx=%d level_list[idx]=%d -> boot_freq=%d\n",
                __func__, table->boot_level_idx,
                domain->level_list[table->boot_level_idx],
                domain->level_list[table->boot_level_idx] * 1000);
        vclk->boot_freq = domain->level_list[table->boot_level_idx] * 1000;
    } else {
        pr_info("VCLK: %s: boot idx=%d -> boot_freq=-1\n", __func__,
                table->boot_level_idx);
        vclk->boot_freq = -1;
    }

    if (table->resume_level_idx >= 0) {
        pr_info(
            "VCLK: %s: resume idx=%d level_list[idx]=%d -> resume_freq=%d\n",
            __func__, table->resume_level_idx,
            domain->level_list[table->resume_level_idx],
            domain->level_list[table->resume_level_idx] * 1000);
        vclk->resume_freq = domain->level_list[table->resume_level_idx] * 1000;
    } else {
        pr_info("VCLK: %s: resume idx=%d -> resume_freq=-1\n", __func__,
                table->resume_level_idx);
        vclk->resume_freq = -1;
    }

minmax_skip:
    pr_info("VCLK: %s: summary name=%s id=%u\n", __func__,
            vclk->name ? vclk->name : "(null)", vclk->id);
    pr_info("VCLK: %s:   num_rates    : %7d\n", __func__, vclk->num_rates);
    pr_info("VCLK: %s:   num_clk_list : %7d\n", __func__, vclk->num_list);
    pr_info("VCLK: %s:   max_freq     : %7d\n", __func__, vclk->max_freq);
    pr_info("VCLK: %s:   min_freq     : %7d\n", __func__, vclk->min_freq);
    pr_info("VCLK: %s:   boot_freq    : %7d\n", __func__, vclk->boot_freq);
    pr_info("VCLK: %s:   resume_freq  : %7d\n", __func__, vclk->resume_freq);
    pr_info("VCLK: %s: exit ret=%d\n", __func__, ret);

    return ret;
}

static void vclk_bind(void) {
    struct vclk *vclk;
    int i;
    bool warn_on = 0;
    int ret;

    for (i = 0; i < cmucal_get_list_size(ACPM_VCLK_TYPE); i++) {
        vclk = cmucal_get_node(ACPM_VCLK_TYPE | i);
        if (!vclk) {
            pr_err("cannot found vclk node %x\n", i);
            continue;
        }

        ret = vclk_get_dfs_info(vclk);
        if (ret == -EVCLKNOENT) {
            if (!warn_on)
                pr_warn("ECT DVFS not found\n");
            warn_on = 1;
        } else if (ret) {
            pr_err("ECT DVFS [%s] not found %d\n", vclk->name, ret);
        } else {
            ret = vclk_get_asv_info(vclk);
            if (ret)
                pr_err("ECT ASV [%s] not found %d\n", vclk->name, ret);
        }
    }
}

int vclk_register_ops(unsigned int id, struct vclk_trans_ops *ops) {
    struct vclk *vclk;

    if (IS_DFS_VCLK(id)) {
        vclk = cmucal_get_node(id);
        if (!vclk)
            return -EVCLKINVAL;
        vclk->ops = ops;

        return 0;
    }

    return -EVCLKNOENT;
}

int __init vclk_initialize(void) {
    pr_info("vclk initialize for cmucal\n");

    ra_init();

    asv_table_ver = asv_table_init();
    id_get_rev(&main_rev, &sub_rev);

    vclk_bind();

    return 0;
}
