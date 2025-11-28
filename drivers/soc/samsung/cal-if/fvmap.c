#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <soc/samsung/cal-if.h>

#include "cmucal.h"
#include "fvmap.h"
#include "gpu_dvfs_overrides.h"
#include "ra.h"
#include "vclk.h"

#define FVMAP_SIZE (SZ_8K)
#define STEP_UV (6250)

void __iomem *fvmap_base;
void __iomem *sram_fvmap_base;

static int init_margin_table[MAX_MARGIN_ID];
static int volt_offset_percent = 0;
static int percent_margin_table[MAX_MARGIN_ID];
static struct vclk_lut *g3d_lut_override;
static size_t g3d_lut_override_cap;

#define G3D_MANUAL_RATE(_mhz, _uv) {.rate = (_mhz) * 1000U, .volt = (_uv)}

static const struct rate_volt g3d_manual_ratevolt[] = {
    G3D_MANUAL_RATE(910, 837500), // 4 140 0 0
    G3D_MANUAL_RATE(858, 812500), // 4 132 0 0
    G3D_MANUAL_RATE(806, 787500), // 4 124 0 0
    G3D_MANUAL_RATE(754, 768750), // 4 116 0 0
    G3D_MANUAL_RATE(702, 750000),
    G3D_MANUAL_RATE(676, 706250),
    G3D_MANUAL_RATE(650, 700000),
    G3D_MANUAL_RATE(598, 681250),
    G3D_MANUAL_RATE(572, 675000),
    G3D_MANUAL_RATE(433, 650000),
    G3D_MANUAL_RATE(377, 637500),
    G3D_MANUAL_RATE(325, 612500),
    G3D_MANUAL_RATE(260, 600000),
    G3D_MANUAL_RATE(200, 593750),
    G3D_MANUAL_RATE(156, 562500),
    G3D_MANUAL_RATE(100, 537500),
};

static size_t g3d_find_closest_lv(const struct rate_volt_header *old_rv,
                                  size_t old_lv, unsigned int target_rate) {
    size_t best = 0;
    size_t j;
    u64 best_diff = ~0ULL;

    for (j = 0; j < old_lv; j++) {
        u64 r = old_rv->table[j].rate;
        u64 diff = (r > target_rate) ? (r - target_rate) : (target_rate - r);
        if (diff < best_diff) {
            best_diff = diff;
            best = j;
        }
    }
    return best;
}

static int g3d_ensure_lut(struct vclk *vclk, size_t manual_lv) {
    size_t i;

    if (!vclk || !manual_lv || !vclk->num_list)
        return -EINVAL;

    if (!g3d_lut_override || g3d_lut_override_cap < manual_lv) {
        struct vclk_lut *new_lut;

        if (g3d_lut_override) {
            for (i = 0; i < g3d_lut_override_cap; i++)
                kfree(g3d_lut_override[i].params);
            kfree(g3d_lut_override);
        }

        new_lut = kcalloc(manual_lv, sizeof(*new_lut), GFP_KERNEL);
        if (!new_lut)
            return -ENOMEM;

        g3d_lut_override = new_lut;
        g3d_lut_override_cap = manual_lv;

        for (i = 0; i < g3d_lut_override_cap; i++) {
            new_lut[i].params = kcalloc(vclk->num_list, sizeof(int), GFP_KERNEL);
            if (!new_lut[i].params)
                goto err_alloc;
        }
    }

    for (i = 0; i < g3d_lut_override_cap; i++)
        memset(g3d_lut_override[i].params, 0,
               sizeof(int) * vclk->num_list);

    vclk->lut = g3d_lut_override;
    return 0;

err_alloc:
    while (i--)
        kfree(g3d_lut_override[i].params);
    kfree(g3d_lut_override);
    g3d_lut_override = NULL;
    g3d_lut_override_cap = 0;
    return -ENOMEM;
}

static int patch_tables(volatile struct fvmap_header *hdr,
                        const struct rate_volt_header *old_rv,
                        const struct dvfs_table *old_param,
                        struct rate_volt_header *new_rv,
                        struct dvfs_table *new_param, struct vclk *vclk,
                        size_t old_lv) {
    size_t manual_lv = ARRAY_SIZE(g3d_manual_ratevolt);
    size_t members = hdr->num_of_members;
    size_t lv, k;

    if (!vclk)
        return -EINVAL;

    if (g3d_ensure_lut(vclk, manual_lv))
        return -ENOMEM;

    vclk->num_rates = manual_lv;
    vclk->max_freq = g3d_manual_ratevolt[0].rate;
    vclk->min_freq = g3d_manual_ratevolt[manual_lv - 1].rate;

    for (lv = 0; lv < manual_lv; lv++) {
        size_t src_lv;
        unsigned int rate = g3d_manual_ratevolt[lv].rate;
        unsigned int volt = g3d_manual_ratevolt[lv].volt;

        new_rv->table[lv].rate = rate;
        new_rv->table[lv].volt = volt;

        vclk->lut[lv].rate = rate;

        if (lv < old_lv)
            src_lv = g3d_find_closest_lv(old_rv, old_lv, rate);
        else if (lv)
            src_lv = lv - 1;
        else
            src_lv = 0;

        for (k = 0; k < members; k++) {
            unsigned int p;

            if (lv < old_lv)
                p = old_param->val[src_lv * members + k];
            else
                p = new_param->val[(lv - 1) * members + k];

            new_param->val[lv * members + k] = p;
            vclk->lut[lv].params[k] = p;
        }
    }

    hdr->num_of_lv = manual_lv;
    return 0;
}

static int __init get_mif_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_MIF] = volt;

    return 0;
}
early_param("mif", get_mif_volt);

static int __init get_int_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_INT] = volt;

    return 0;
}
early_param("int", get_int_volt);

static int __init get_big_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_BIG] = volt;

    return 0;
}
early_param("big", get_big_volt);

static int __init get_mid_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_MID] = volt;

    return 0;
}
early_param("mid", get_mid_volt);

static int __init get_lit_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_LIT] = volt;

    return 0;
}
early_param("lit", get_lit_volt);

static int __init get_g3d_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_G3D] = volt;

    return 0;
}
early_param("g3d", get_g3d_volt);

static int __init get_intcam_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_INTCAM] = volt;

    return 0;
}
early_param("intcam", get_intcam_volt);

static int __init get_cam_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_CAM] = volt;

    return 0;
}
early_param("cam", get_cam_volt);

static int __init get_disp_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_DISP] = volt;

    return 0;
}
early_param("disp", get_disp_volt);

static int __init get_g3dm_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_G3DM] = volt;

    return 0;
}
early_param("g3dm", get_g3dm_volt);

static int __init get_cp_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_CP] = volt;

    return 0;
}
early_param("cp", get_cp_volt);

static int __init get_fsys0_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_FSYS0] = volt;

    return 0;
}
early_param("fsys0", get_fsys0_volt);

static int __init get_aud_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_AUD] = volt;

    return 0;
}
early_param("aud", get_aud_volt);

static int __init get_iva_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_IVA] = volt;

    return 0;
}
early_param("iva", get_iva_volt);

static int __init get_score_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_SCORE] = volt;

    return 0;
}
early_param("score", get_score_volt);

static int __init get_npu_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_NPU] = volt;

    return 0;
}
early_param("npu", get_npu_volt);

static int __init get_mfc_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_MFC] = volt;

    return 0;
}
early_param("mfc", get_mfc_volt);

static int __init get_percent_margin_volt(char *str) {
    int percent;

    get_option(&str, &percent);
    volt_offset_percent = percent;

    return 0;
}
early_param("volt_offset_percent", get_percent_margin_volt);

int fvmap_set_raw_voltage_table(unsigned int id, int uV) {
    struct fvmap_header *fvmap_header;
    struct rate_volt_header *fv_table;
    int num_of_lv;
    int idx, i;

    idx = GET_IDX(id);

    fvmap_header = sram_fvmap_base;
    fv_table = sram_fvmap_base + fvmap_header[idx].o_ratevolt;
    num_of_lv = fvmap_header[idx].num_of_lv;

    for (i = 0; i < num_of_lv; i++)
        fv_table->table[i].volt += uV;

    return 0;
}

int fvmap_get_voltage_table(unsigned int id, unsigned int *table) {
    struct fvmap_header *fvmap_header = fvmap_base;
    struct rate_volt_header *fv_table;
    int idx, i;
    int num_of_lv;

    if (!IS_ACPM_VCLK(id))
        return 0;

    idx = GET_IDX(id);

    fvmap_header = fvmap_base;
    fv_table = fvmap_base + fvmap_header[idx].o_ratevolt;
    num_of_lv = fvmap_header[idx].num_of_lv;

    for (i = 0; i < num_of_lv; i++)
        table[i] = fv_table->table[i].volt;

    return num_of_lv;
}

int fvmap_get_raw_voltage_table(unsigned int id) {
    struct fvmap_header *fvmap_header;
    struct rate_volt_header *fv_table;
    int idx, i;
    int num_of_lv;
    unsigned int table[20];

    idx = GET_IDX(id);

    fvmap_header = sram_fvmap_base;
    fv_table = sram_fvmap_base + fvmap_header[idx].o_ratevolt;
    num_of_lv = fvmap_header[idx].num_of_lv;

    for (i = 0; i < num_of_lv; i++)
        table[i] = fv_table->table[i].volt;

    for (i = 0; i < num_of_lv; i++)
        printk("dvfs id : %d  %d Khz : %d uv\n", ACPM_VCLK_TYPE | id,
               fv_table->table[i].rate, table[i]);

    return 0;
}

static void check_percent_margin(struct rate_volt_header *head,
                                 unsigned int num_of_lv) {
    int org_volt;
    int percent_volt;
    int i;

    if (!volt_offset_percent)
        return;

    for (i = 0; i < num_of_lv; i++) {
        org_volt = head->table[i].volt;
        percent_volt = org_volt * volt_offset_percent / 100;
        head->table[i].volt = org_volt + rounddown(percent_volt, STEP_UV);
    }
}

static int get_vclk_id_from_margin_id(int margin_id) {
    int size = cmucal_get_list_size(ACPM_VCLK_TYPE);
    int i;
    struct vclk *vclk;

    for (i = 0; i < size; i++) {
        vclk = cmucal_get_node(ACPM_VCLK_TYPE | i);

        if (vclk->margin_id == margin_id)
            return i;
    }

    return -EINVAL;
}

#define attr_percent(margin_id, type)                                          \
    static ssize_t show_##type##_percent(                                      \
        struct kobject *kobj, struct kobj_attribute *attr, char *buf) {        \
        return snprintf(buf, PAGE_SIZE, "%d\n",                                \
                        percent_margin_table[margin_id]);                      \
    }                                                                          \
                                                                               \
    static ssize_t store_##type##_percent(struct kobject *kobj,                \
                                          struct kobj_attribute *attr,         \
                                          const char *buf, size_t count) {     \
        int input, vclk_id;                                                    \
                                                                               \
        if (!sscanf(buf, "%d", &input))                                        \
            return -EINVAL;                                                    \
                                                                               \
        if (input < -100 || input > 100)                                       \
            return -EINVAL;                                                    \
                                                                               \
        vclk_id = get_vclk_id_from_margin_id(margin_id);                       \
        if (vclk_id == -EINVAL)                                                \
            return vclk_id;                                                    \
        percent_margin_table[margin_id] = input;                               \
        cal_dfs_set_volt_margin(vclk_id | ACPM_VCLK_TYPE, input);              \
                                                                               \
        return count;                                                          \
    }                                                                          \
                                                                               \
    static struct kobj_attribute type##_percent = __ATTR(                      \
        type##_percent, 0600, show_##type##_percent, store_##type##_percent)

attr_percent(MARGIN_MIF, mif_margin);
attr_percent(MARGIN_INT, int_margin);
attr_percent(MARGIN_BIG, big_margin);
attr_percent(MARGIN_MID, mid_margin);
attr_percent(MARGIN_LIT, lit_margin);
attr_percent(MARGIN_G3D, g3d_margin);
attr_percent(MARGIN_INTCAM, intcam_margin);
attr_percent(MARGIN_CAM, cam_margin);
attr_percent(MARGIN_DISP, disp_margin);
attr_percent(MARGIN_CP, cp_margin);
attr_percent(MARGIN_FSYS0, fsys0_margin);
attr_percent(MARGIN_AUD, aud_margin);
attr_percent(MARGIN_IVA, iva_margin);
attr_percent(MARGIN_SCORE, score_margin);
attr_percent(MARGIN_NPU, npu_margin);
attr_percent(MARGIN_MFC, mfc_margin);

static struct attribute *percent_margin_attrs[] = {
    &mif_margin_percent.attr,
    &int_margin_percent.attr,
    &big_margin_percent.attr,
    &mid_margin_percent.attr,
    &lit_margin_percent.attr,
    &g3d_margin_percent.attr,
    &intcam_margin_percent.attr,
    &cam_margin_percent.attr,
    &disp_margin_percent.attr,
    &cp_margin_percent.attr,
    &fsys0_margin_percent.attr,
    &aud_margin_percent.attr,
    &iva_margin_percent.attr,
    &score_margin_percent.attr,
    &npu_margin_percent.attr,
    &mfc_margin_percent.attr,
    NULL,
};

static const struct attribute_group percent_margin_group = {
    .attrs = percent_margin_attrs,
};

static void fvmap_copy_from_sram(void __iomem *map_base,
                                 void __iomem *sram_base) {
    volatile struct fvmap_header *fvmap_header, *header;
    struct rate_volt_header *old, *new;
    struct dvfs_table *old_param, *new_param;
    struct clocks *clks;
    struct pll_header *plls;
    struct vclk *vclk;
    unsigned int member_addr;
    unsigned int blk_idx, param_idx;
    int size, margin;
    int i, j, k;
    bool is_g3d;
    size_t old_lv;

    fvmap_header = map_base;
    header = sram_base;

    size = cmucal_get_list_size(ACPM_VCLK_TYPE);

    for (i = 0; i < size; i++) {
        /* load fvmap info */
        fvmap_header[i].dvfs_type = header[i].dvfs_type;
        fvmap_header[i].num_of_lv = header[i].num_of_lv;
        fvmap_header[i].num_of_members = header[i].num_of_members;
        fvmap_header[i].num_of_pll = header[i].num_of_pll;
        fvmap_header[i].num_of_mux = header[i].num_of_mux;
        fvmap_header[i].num_of_div = header[i].num_of_div;
        fvmap_header[i].gearratio = header[i].gearratio;
        fvmap_header[i].init_lv = header[i].init_lv;
        fvmap_header[i].num_of_gate = header[i].num_of_gate;
        fvmap_header[i].reserved[0] = header[i].reserved[0];
        fvmap_header[i].reserved[1] = header[i].reserved[1];
        fvmap_header[i].block_addr[0] = header[i].block_addr[0];
        fvmap_header[i].block_addr[1] = header[i].block_addr[1];
        fvmap_header[i].block_addr[2] = header[i].block_addr[2];
        fvmap_header[i].o_members = header[i].o_members;
        fvmap_header[i].o_ratevolt = header[i].o_ratevolt;
        fvmap_header[i].o_tables = header[i].o_tables;

        vclk = cmucal_get_node(ACPM_VCLK_TYPE | i);
        if (vclk == NULL)
            continue;

        // expand listed size
        is_g3d = !strcmp(vclk->name, "dvfs_g3d");
        old_lv = fvmap_header[i].num_of_lv;

        if (is_g3d) {
            fvmap_header[i].num_of_lv = ARRAY_SIZE(g3d_manual_ratevolt);
        }

        pr_info("dvfs_type : %s - id : %x\n", vclk->name,
                fvmap_header[i].dvfs_type);
        pr_info("  num_of_lv      : %d\n", fvmap_header[i].num_of_lv);
        pr_info("  num_of_members : %d\n", fvmap_header[i].num_of_members);

        old = sram_base + fvmap_header[i].o_ratevolt;
        new = map_base + fvmap_header[i].o_ratevolt;

        check_percent_margin(old, old_lv);

        margin = init_margin_table[vclk->margin_id];
        if (margin)
            cal_dfs_set_volt_margin(i | ACPM_VCLK_TYPE, margin);

        for (j = 0; j < fvmap_header[i].num_of_members; j++) {
            clks = sram_base + fvmap_header[i].o_members;

            if (j < fvmap_header[i].num_of_pll) {
                plls = sram_base + clks->addr[j];
                member_addr = plls->addr - 0x90000000;
            } else {

                member_addr = (clks->addr[j] & ~0x3) & 0xffff;
                blk_idx = clks->addr[j] & 0x3;

                if (blk_idx < BLOCK_ADDR_SIZE)
                    member_addr |=
                        ((fvmap_header[i].block_addr[blk_idx]) << 16) -
                        0x90000000;
                else
                    pr_err("[%s] blk_idx %u is out of range for block_addr\n",
                           __func__, blk_idx);
            }

            vclk->list[j] = cmucal_get_id_by_addr(member_addr);

            if (vclk->list[j] == INVALID_CLK_ID)
                pr_info("  Invalid addr :0x%x\n", member_addr);
            else
                pr_info("  DVFS CMU addr:0x%x\n", member_addr);
        }

        // patch levels
        if (!strcmp(vclk->name, "dvfs_g3d")) {

            for (j = 0; j < fvmap_header[i].num_of_lv; j++) {
                new->table[j].rate = g3d_manual_ratevolt[j].rate;
                new->table[j].volt = g3d_manual_ratevolt[j].volt;
                pr_info("  lv g3d : [%7d], volt = %d uV (%d %%) \n",
                        new->table[j].rate, new->table[j].volt,
                        volt_offset_percent);
            }

        } else {
            for (j = 0; j < fvmap_header[i].num_of_lv; j++) {
                new->table[j].rate = old->table[j].rate;
                new->table[j].volt = old->table[j].volt;
                pr_info("  lv : [%7d], volt = %d uV (%d %%) \n",
                        new->table[j].rate, new->table[j].volt,
                        volt_offset_percent);
            }
        }

        old_param = sram_base + fvmap_header[i].o_tables;
        new_param = map_base + fvmap_header[i].o_tables;

        // patch vclk
        if (!strcmp(vclk->name, "dvfs_g3d")) {
            int ret = patch_tables(&fvmap_header[i], old, old_param, new,
                                   new_param, vclk, old_lv);
            if (ret)
                pr_err("G3D: manual override failed: %d\n", ret);
            continue;
        }

        // each raw
        for (j = 0; j < fvmap_header[i].num_of_lv; j++) {

            // each entry
            for (k = 0; k < fvmap_header[i].num_of_members; k++) {

                param_idx = fvmap_header[i].num_of_members * j + k;

                new_param->val[param_idx] = old_param->val[param_idx];

                if (vclk->lut[j].params[k] != new_param->val[param_idx]) {

                    vclk->lut[j].params[k] = new_param->val[param_idx];

                    pr_info("Mis-match %s[%d][%d] : %d %d\n", vclk->name, j, k,
                            vclk->lut[j].params[k], new_param->val[param_idx]);
                }
            }
        }
    }
}

int fvmap_init(void __iomem *sram_base) {
    void __iomem *map_base;
    struct kobject *kobj;

    map_base = kzalloc(FVMAP_SIZE, GFP_KERNEL);

    fvmap_base = map_base;
    sram_fvmap_base = sram_base;
    pr_info("%s:fvmap initialize %p\n", __func__, sram_base);
    fvmap_copy_from_sram(map_base, sram_base);

    /* percent margin for each doamin at runtime */
    kobj = kobject_create_and_add("percent_margin", power_kobj);
    if (!kobj)
        pr_err("Fail to create percent_margin kboject\n");

    if (sysfs_create_group(kobj, &percent_margin_group))
        pr_err("Fail to create percent_margin group\n");

    return 0;
}
