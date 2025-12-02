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
#include <linux/errno.h>
#include <linux/printk.h>
#include <linux/types.h>

#include <linux/io.h>
#include <linux/printk.h>
#include <linux/slab.h>

#define FVMAP_DUMP_MAX_LV 2048
#define FVMAP_DUMP_MAX_MEM 2048

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

    pr_info("%s: old_lv=%zu target_rate=%u\n", __func__, old_lv, target_rate);

    for (j = 0; j < old_lv; j++) {
        u64 r = old_rv->table[j].rate;
        u64 diff = (r > target_rate) ? (r - target_rate) : (target_rate - r);

        pr_info("%s: checking index=%zu rate=%llu diff=%llu\n", __func__, j, r,
                diff);
        if (diff < best_diff) {
            best_diff = diff;
            best = j;
        }
    }
    pr_info("%s: best index=%zu best_diff=%llu\n", __func__, best, best_diff);
    return best;
}

static int g3d_ensure_lut(struct vclk *vclk, size_t manual_lv) {
    size_t i;

    pr_info("%s: vclk=%p manual_lv=%zu num_list=%zu override_cap=%zu\n",
            __func__, vclk, manual_lv, vclk ? vclk->num_list : 0,
            g3d_lut_override_cap);

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
            new_lut[i].params =
                kcalloc(vclk->num_list, sizeof(int), GFP_KERNEL);
            if (!new_lut[i].params)
                goto err_alloc;

            pr_info("%s: allocated params for level=%zu size=%zu\n", __func__,
                    i, vclk->num_list);
        }
    }

    for (i = 0; i < g3d_lut_override_cap; i++)
        memset(g3d_lut_override[i].params, 0, sizeof(int) * vclk->num_list);

    vclk->lut = g3d_lut_override;
    pr_info("%s: override prepared cap=%zu\n", __func__, g3d_lut_override_cap);
    return 0;

err_alloc:
    while (i--)
        kfree(g3d_lut_override[i].params);
    kfree(g3d_lut_override);
    g3d_lut_override = NULL;
    g3d_lut_override_cap = 0;
    pr_info("%s: allocation failed at level=%zu\n", __func__, i);
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

    pr_info("%s: members=%zu manual_lv=%zu old_lv=%zu\n", __func__, members,
            manual_lv, old_lv);

    if (!vclk)
        return -EINVAL;

    if (g3d_ensure_lut(vclk, manual_lv))
        return -ENOMEM;

    vclk->num_rates = manual_lv;
    vclk->max_freq = g3d_manual_ratevolt[0].rate;
    vclk->min_freq = g3d_manual_ratevolt[manual_lv - 1].rate;

    pr_info("%s: updated vclk rates max=%u min=%u\n", __func__, vclk->max_freq,
            vclk->min_freq);

    for (lv = 0; lv < manual_lv; lv++) {
        size_t src_lv;
        unsigned int rate = g3d_manual_ratevolt[lv].rate;
        unsigned int volt = g3d_manual_ratevolt[lv].volt;

        pr_info("%s: processing lv=%zu rate=%u volt=%u\n", __func__, lv, rate,
                volt);

        new_rv->table[lv].rate = rate;
        new_rv->table[lv].volt = volt;

        vclk->lut[lv].rate = rate;

        if (lv < old_lv)
            src_lv = g3d_find_closest_lv(old_rv, old_lv, rate);
        else if (lv)
            src_lv = lv - 1;
        else
            src_lv = 0;

        pr_info("%s: source level for lv=%zu is %zu\n", __func__, lv, src_lv);

        for (k = 0; k < members; k++) {
            unsigned int p;

            if (lv < old_lv)
                p = old_param->val[src_lv * members + k];
            else
                p = new_param->val[(lv - 1) * members + k];

            new_param->val[lv * members + k] = p;
            vclk->lut[lv].params[k] = p;

            pr_info("%s: lv=%zu member=%zu param=%u\n", __func__, lv, k, p);
        }
    }

    hdr->num_of_lv = manual_lv;
    pr_info("%s: patched num_of_lv=%zu\n", __func__, manual_lv);
    return 0;
}

static int __init get_mif_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_MIF] = volt;

    pr_info("%s: str=%s volt=%d\n", __func__, str, volt);

    return 0;
}
early_param("mif", get_mif_volt);

static int __init get_int_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_INT] = volt;

    pr_info("%s: str=%s volt=%d\n", __func__, str, volt);

    return 0;
}
early_param("int", get_int_volt);

static int __init get_big_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_BIG] = volt;

    pr_info("%s: str=%s volt=%d\n", __func__, str, volt);

    return 0;
}
early_param("big", get_big_volt);

static int __init get_mid_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_MID] = volt;

    pr_info("%s: str=%s volt=%d\n", __func__, str, volt);

    return 0;
}
early_param("mid", get_mid_volt);

static int __init get_lit_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_LIT] = volt;

    pr_info("%s: str=%s volt=%d\n", __func__, str, volt);

    return 0;
}
early_param("lit", get_lit_volt);

static int __init get_g3d_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_G3D] = volt;

    pr_info("%s: str=%s volt=%d\n", __func__, str, volt);

    return 0;
}
early_param("g3d", get_g3d_volt);

static int __init get_intcam_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_INTCAM] = volt;

    pr_info("%s: str=%s volt=%d\n", __func__, str, volt);

    return 0;
}
early_param("intcam", get_intcam_volt);

static int __init get_cam_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_CAM] = volt;

    pr_info("%s: str=%s volt=%d\n", __func__, str, volt);

    return 0;
}
early_param("cam", get_cam_volt);

static int __init get_disp_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_DISP] = volt;

    pr_info("%s: str=%s volt=%d\n", __func__, str, volt);

    return 0;
}
early_param("disp", get_disp_volt);

static int __init get_g3dm_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_G3DM] = volt;

    pr_info("%s: str=%s volt=%d\n", __func__, str, volt);

    return 0;
}
early_param("g3dm", get_g3dm_volt);

static int __init get_cp_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_CP] = volt;

    pr_info("%s: str=%s volt=%d\n", __func__, str, volt);

    return 0;
}
early_param("cp", get_cp_volt);

static int __init get_fsys0_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_FSYS0] = volt;

    pr_info("%s: str=%s volt=%d\n", __func__, str, volt);

    return 0;
}
early_param("fsys0", get_fsys0_volt);

static int __init get_aud_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_AUD] = volt;

    pr_info("%s: str=%s volt=%d\n", __func__, str, volt);

    return 0;
}
early_param("aud", get_aud_volt);

static int __init get_iva_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_IVA] = volt;

    pr_info("%s: str=%s volt=%d\n", __func__, str, volt);

    return 0;
}
early_param("iva", get_iva_volt);

static int __init get_score_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_SCORE] = volt;

    pr_info("%s: str=%s volt=%d\n", __func__, str, volt);

    return 0;
}
early_param("score", get_score_volt);

static int __init get_npu_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_NPU] = volt;

    pr_info("%s: str=%s volt=%d\n", __func__, str, volt);

    return 0;
}
early_param("npu", get_npu_volt);

static int __init get_mfc_volt(char *str) {
    int volt;

    get_option(&str, &volt);
    init_margin_table[MARGIN_MFC] = volt;

    pr_info("%s: str=%s volt=%d\n", __func__, str, volt);

    return 0;
}
early_param("mfc", get_mfc_volt);

static int __init get_percent_margin_volt(char *str) {
    int percent;

    get_option(&str, &percent);
    volt_offset_percent = percent;

    pr_info("%s: str=%s percent=%d\n", __func__, str, percent);

    return 0;
}
early_param("volt_offset_percent", get_percent_margin_volt);

static inline void *fvmap_ptr_add(void *base, u32 byte_off) {
    return (void *)((u8 *)base + byte_off);
}

int fvmap_set_raw_voltage_table(unsigned int id, int delta_uV) {
    struct fvmap_header *hdr;
    struct rate_volt_header *rv;
    int idx, i, num_lv;

    if (!IS_ACPM_VCLK(id))
        return -EINVAL;

    if (!sram_fvmap_base)
        return -ENODEV;

    idx = GET_IDX(id);
    hdr = (struct fvmap_header *)sram_fvmap_base;

    /* If you have a header-count field, validate idx here. Example:
     * if (idx < 0 || idx >= hdr->num_of_domain) return -EINVAL;
     */

    rv = (struct rate_volt_header *)fvmap_ptr_add(sram_fvmap_base,
                                                  hdr[idx].o_ratevolt);
    num_lv = hdr[idx].num_of_lv;

    if (num_lv <= 0) {
        pr_err("%s: id=%u idx=%d invalid num_of_lv=%d\n", __func__, id, idx,
               num_lv);
        return -EINVAL;
    }

    pr_info("%s: id=%u idx=%d num_of_lv=%d delta_uV=%d\n", __func__, id, idx,
            num_lv, delta_uV);

    for (i = 0; i < num_lv; i++) {
        u32 old_uV = rv->table[i].volt;
        s64 new_s64 = (s64)old_uV + (s64)delta_uV;
        u32 new_uV;

        if (new_s64 < 0) {
            pr_warn(
                "%s: lv=%d old=%u delta=%d => negative (%lld), clamping to 0\n",
                __func__, i, old_uV, delta_uV, new_s64);
            new_uV = 0;
        } else if (new_s64 > U32_MAX) {
            pr_warn("%s: lv=%d old=%u delta=%d => overflow (%lld), clamping to "
                    "U32_MAX\n",
                    __func__, i, old_uV, delta_uV, new_s64);
            new_uV = U32_MAX;
        } else {
            new_uV = (u32)new_s64;
        }

        pr_info("%s: lv=%d rate=%u old_uV=%u new_uV=%u\n", __func__, i,
                rv->table[i].rate, old_uV, new_uV);

        rv->table[i].volt = new_uV;
    }

    return 0;
}

int fvmap_get_voltage_table(unsigned int id, unsigned int *table) {
    struct fvmap_header *hdr;
    struct rate_volt_header *rv;
    int idx, i, num_lv;

    if (!IS_ACPM_VCLK(id))
        return -EINVAL;

    if (!table)
        return -EINVAL;

    if (!fvmap_base)
        return -ENODEV;

    idx = GET_IDX(id);
    hdr = (struct fvmap_header *)fvmap_base;

    rv = (struct rate_volt_header *)fvmap_ptr_add(fvmap_base,
                                                  hdr[idx].o_ratevolt);
    num_lv = hdr[idx].num_of_lv;

    if (num_lv <= 0) {
        pr_err("%s: id=%u idx=%d invalid num_of_lv=%d\n", __func__, id, idx,
               num_lv);
        return -EINVAL;
    }

    pr_info("%s: id=%u idx=%d num_of_lv=%d\n", __func__, id, idx, num_lv);

    for (i = 0; i < num_lv; i++)
        table[i] = rv->table[i].volt;

    for (i = 0; i < num_lv; i++)
        pr_info("%s: lv=%d rate=%u volt_uV=%u\n", __func__, i,
                rv->table[i].rate, table[i]);

    return num_lv;
}

int fvmap_get_raw_voltage_table(unsigned int id) {
    struct fvmap_header *hdr;
    struct rate_volt_header *rv;
    int idx, i, num_lv;

    if (!IS_ACPM_VCLK(id))
        return -EINVAL;

    if (!sram_fvmap_base)
        return -ENODEV;

    idx = GET_IDX(id);
    hdr = (struct fvmap_header *)sram_fvmap_base;

    rv = (struct rate_volt_header *)fvmap_ptr_add(sram_fvmap_base,
                                                  hdr[idx].o_ratevolt);
    num_lv = hdr[idx].num_of_lv;

    if (num_lv <= 0) {
        pr_err("%s: id=%u idx=%d invalid num_of_lv=%d\n", __func__, id, idx,
               num_lv);
        return -EINVAL;
    }

    pr_info("%s: id=%u idx=%d num_of_lv=%d\n", __func__, id, idx, num_lv);

    for (i = 0; i < num_lv; i++)
        pr_info("dvfs id:%u rate_kHz:%u volt_uV:%u\n",
                (unsigned int)(ACPM_VCLK_TYPE | id), rv->table[i].rate,
                rv->table[i].volt);

    return 0;
}

static void check_percent_margin(struct rate_volt_header *head,
                                 unsigned int num_of_lv) {
    int org_volt;
    int percent_volt;
    int i;

    pr_info("%s: num_of_lv=%u volt_offset_percent=%d\n", __func__, num_of_lv,
            volt_offset_percent);

    if (!volt_offset_percent)
        return;

    for (i = 0; i < num_of_lv; i++) {
        org_volt = head->table[i].volt;
        percent_volt = org_volt * volt_offset_percent / 100;
        head->table[i].volt = org_volt + rounddown(percent_volt, STEP_UV);

        pr_info("%s: lv=%d org=%d percent=%d updated=%d\n", __func__, i,
                org_volt, percent_volt, head->table[i].volt);
    }
}

static int get_vclk_id_from_margin_id(int margin_id) {
    int size = cmucal_get_list_size(ACPM_VCLK_TYPE);
    int i;
    struct vclk *vclk;

    pr_info("%s: margin_id=%d size=%d\n", __func__, margin_id, size);

    for (i = 0; i < size; i++) {
        vclk = cmucal_get_node(ACPM_VCLK_TYPE | i);

        pr_info("%s: checking index=%d name=%s margin_id=%d\n", __func__, i,
                vclk ? vclk->name : "NULL", vclk ? vclk->margin_id : -1);

        if (vclk->margin_id == margin_id)
            return i;
    }

    return -EINVAL;
}

#define attr_percent(margin_id, type)                                          \
    static ssize_t show_##type##_percent(                                      \
        struct kobject *kobj, struct kobj_attribute *attr, char *buf) {        \
        pr_info("show_%s_percent: value=%d\n", #type,                          \
                percent_margin_table[margin_id]);                              \
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
        pr_info("store_%s_percent: input=%d\n", #type, input);                 \
        if (input < -100 || input > 100)                                       \
            return -EINVAL;                                                    \
                                                                               \
        vclk_id = get_vclk_id_from_margin_id(margin_id);                       \
        if (vclk_id == -EINVAL)                                                \
            return vclk_id;                                                    \
        percent_margin_table[margin_id] = input;                               \
        cal_dfs_set_volt_margin(vclk_id | ACPM_VCLK_TYPE, input);              \
        pr_info("store_%s_percent: vclk_id=%d updated=%d\n", #type, vclk_id,   \
                input);                                                        \
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

static inline void __iomem *io_ptr_add(void __iomem *base, u32 byte_off) {
    return (void __iomem *)((u8 __iomem *)base + byte_off);
}

static void fvmap_dump_header_entry(const char *tag, int idx,
                                    const struct fvmap_header *h) {
    pr_info("FVMAP[%s] idx=%d dvfs_type=%u num_lv=%u members=%u pll=%u mux=%u "
            "div=%u gear=%u init_lv=%u gate=%u\n",
            tag, idx, h->dvfs_type, h->num_of_lv, h->num_of_members,
            h->num_of_pll, h->num_of_mux, h->num_of_div, h->gearratio,
            h->init_lv, h->num_of_gate);

    pr_info("FVMAP[%s] idx=%d reserved[0]=0x%x reserved[1]=0x%x\n", tag, idx,
            h->reserved[0], h->reserved[1]);

    pr_info("FVMAP[%s] idx=%d block_addr[0..2]=0x%x 0x%x 0x%x\n", tag, idx,
            h->block_addr[0], h->block_addr[1], h->block_addr[2]);

    pr_info("FVMAP[%s] idx=%d offsets: o_members=0x%x o_ratevolt=0x%x "
            "o_tables=0x%x\n",
            tag, idx, h->o_members, h->o_ratevolt, h->o_tables);
}

/* Safe: copy header entry from __iomem then dump */
static void fvmap_dump_header_entry_io(const char *tag, int idx,
                                       void __iomem *base, u32 byte_off) {
    struct fvmap_header tmp;

    memcpy_fromio(&tmp, io_ptr_add(base, byte_off), sizeof(tmp));
    fvmap_dump_header_entry(tag, idx, &tmp);

    /* Optional: raw bytes */
    print_hex_dump(KERN_INFO, "FVMAP_HDR_BYTES: ", DUMP_PREFIX_OFFSET, 16, 4,
                   &tmp, sizeof(tmp), false);
}

static void fvmap_dump_ratevolt_io(const char *tag, int idx, void __iomem *base,
                                   u32 rv_off, int num_lv) {
    int j, dump_lv;
    struct rate_volt_header *tmp;
    size_t bytes;

    dump_lv = min(num_lv, FVMAP_DUMP_MAX_LV);
    bytes = sizeof(*tmp) + (size_t)dump_lv * sizeof(tmp->table[0]);

    tmp = kzalloc(bytes, GFP_KERNEL);
    if (!tmp) {
        pr_err("FVMAP[%s] idx=%d ratevolt: kzalloc(%zu) failed\n", tag, idx,
               bytes);
        return;
    }

    memcpy_fromio(tmp, io_ptr_add(base, rv_off), bytes);

    pr_info("FVMAP[%s] idx=%d ratevolt_off=0x%x num_lv=%d (dump=%d)\n", tag,
            idx, rv_off, num_lv, dump_lv);

    for (j = 0; j < dump_lv; j++)
        pr_info("FVMAP[%s] idx=%d lv=%d rate=%u volt_uV=%u\n", tag, idx, j,
                tmp->table[j].rate, tmp->table[j].volt);

    /* Optional: raw bytes of the copied blob */
    print_hex_dump(KERN_INFO, "FVMAP_RV_BYTES: ", DUMP_PREFIX_OFFSET, 16, 4,
                   tmp, bytes, false);

    kfree(tmp);
}

static void fvmap_dump_members_io(const char *tag, int idx, void __iomem *base,
                                  u16 mem_off, int num_members, int num_pll,
                                  volatile unsigned short block_addr[BLOCK_ADDR_SIZE]) {
    int j, dump_mem = min(num_members, FVMAP_DUMP_MAX_MEM);

    pr_info("FVMAP[%s] idx=%d members_off=0x%x num_members=%d (dump=%d) "
            "num_pll=%d\n",
            tag, idx, mem_off, num_members, dump_mem, num_pll);

    for (j = 0; j < dump_mem; j++) {
        u32 raw;
        u32 lo, member_addr, blk_idx;

        raw = readl_relaxed((void __iomem *)((u8 __iomem *)base + mem_off +
                                             offsetof(struct clocks, addr) +
                                             j * sizeof(u32)));

        if (j < num_pll) {
            pr_info("FVMAP[%s] idx=%d member=%d clocks.addr_raw=0x%08x (PLL "
                    "entry)\n",
                    tag, idx, j, raw);
            continue;
        }

        lo = (raw & ~0x3u) & 0xffffu;
        blk_idx = raw & 0x3u;

        pr_info("FVMAP[%s] idx=%d member=%d raw=0x%08x lo=0x%04x blk_idx=%u "
                "blk_base_u16=0x%04x\n",
                tag, idx, j, raw, lo, blk_idx,
                (blk_idx < BLOCK_ADDR_SIZE) ? block_addr[blk_idx] : 0);

        if (blk_idx < BLOCK_ADDR_SIZE) {
            u32 hi = ((u32)block_addr[blk_idx]) << 16;
            member_addr = (hi | lo);
            member_addr -= 0x90000000u;

            pr_info("FVMAP[%s] idx=%d member=%d decoded member_addr=0x%08x\n",
                    tag, idx, j, member_addr);
        } else {
            pr_err("FVMAP[%s] idx=%d member=%d blk_idx=%u out of range "
                   "(BLOCK_ADDR_SIZE=%d)\n",
                   tag, idx, j, blk_idx, BLOCK_ADDR_SIZE);
        }
    }
}

static void fvmap_dump_params_io(const char *tag, int idx, void __iomem *base,
                                 u32 tbl_off, int num_lv, int num_members) {
    int j, k, dump_lv, dump_mem;
    struct dvfs_table *tmp;
    size_t total, bytes;

    dump_lv = min(num_lv, FVMAP_DUMP_MAX_LV);
    dump_mem = min(num_members, FVMAP_DUMP_MAX_MEM);
    total = (size_t)dump_lv * (size_t)dump_mem;

    /* dvfs_table layout varies; this assumes dvfs_table has val[] following
       header. If dvfs_table has only val[], sizeof(*tmp) already includes
       val[0] or not depending on definition. This is still typically what your
       existing code relies on (old_param->val[param_idx]). */
    bytes = sizeof(*tmp) + total * sizeof(tmp->val[0]);

    tmp = kzalloc(bytes, GFP_KERNEL);
    if (!tmp) {
        pr_err("FVMAP[%s] idx=%d params: kzalloc(%zu) failed\n", tag, idx,
               bytes);
        return;
    }

    memcpy_fromio(tmp, io_ptr_add(base, tbl_off), bytes);

    pr_info("FVMAP[%s] idx=%d tables_off=0x%x lv=%d(mem=%d) dump_lv=%d "
            "dump_mem=%d\n",
            tag, idx, tbl_off, num_lv, num_members, dump_lv, dump_mem);

    for (j = 0; j < dump_lv; j++) {
        for (k = 0; k < dump_mem; k++) {
            u32 param_idx = (u32)(num_members * j + k);
            /* When dumping only part of members, param_idx still uses full
               stride (num_members), but our copied buffer only includes
               dump_mem*dump_lv; so we also compute a local index. */
            u32 local_idx = (u32)(dump_mem * j + k);

            pr_info("FVMAP[%s] idx=%d lv=%d member=%d param_idx=%u val=%d\n",
                    tag, idx, j, k, param_idx, tmp->val[local_idx]);
        }
    }

    print_hex_dump(KERN_INFO, "FVMAP_TBL_BYTES: ", DUMP_PREFIX_OFFSET, 16, 4,
                   tmp, bytes, false);

    kfree(tmp);
}

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

    pr_info("%s: map_base=%p sram_base=%p\n", __func__, map_base, sram_base);

    fvmap_header = map_base;
    header = sram_base;

    size = cmucal_get_list_size(ACPM_VCLK_TYPE);
    pr_info("%s: total size=%d\n", __func__, size);

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

        /* Dump SRAM header entry (source) and map header entry (dest) */
        fvmap_dump_header_entry("MAP(after_copy)", i,
                                (const struct fvmap_header *)&fvmap_header[i]);
        fvmap_dump_header_entry_io("SRAM(source)", i, sram_base,
                                   i * sizeof(struct fvmap_header));

        /* Dump members, rate/volt and tables from BOTH regions */
        fvmap_dump_members_io("SRAM", i, sram_base, fvmap_header[i].o_members,
                              fvmap_header[i].num_of_members,
                              fvmap_header[i].num_of_pll,
                              fvmap_header[i].block_addr);

        fvmap_dump_members_io("MAP", i, map_base, fvmap_header[i].o_members,
                              fvmap_header[i].num_of_members,
                              fvmap_header[i].num_of_pll,
                              fvmap_header[i].block_addr);

        fvmap_dump_ratevolt_io("SRAM", i, sram_base, fvmap_header[i].o_ratevolt,
                               old_lv);
        fvmap_dump_ratevolt_io("MAP", i, map_base, fvmap_header[i].o_ratevolt,
                               fvmap_header[i].num_of_lv);

        fvmap_dump_params_io("SRAM", i, sram_base, fvmap_header[i].o_tables,
                             old_lv, fvmap_header[i].num_of_members);
        fvmap_dump_params_io("MAP", i, map_base, fvmap_header[i].o_tables,
                             fvmap_header[i].num_of_lv,
                             fvmap_header[i].num_of_members);

        pr_info("%s: index=%d dvfs_type=%d num_lv=%d members=%d\n", __func__, i,
                fvmap_header[i].dvfs_type, fvmap_header[i].num_of_lv,
                fvmap_header[i].num_of_members);

        vclk = cmucal_get_node(ACPM_VCLK_TYPE | i);
        if (vclk == NULL)
            continue;

        // expand listed size
        is_g3d = !strcmp(vclk->name, "dvfs_g3d");
        old_lv = fvmap_header[i].num_of_lv;

        if (is_g3d) {
            fvmap_header[i].num_of_lv = ARRAY_SIZE(g3d_manual_ratevolt);
        }

        pr_info("%s: vclk=%s is_g3d=%d old_lv=%zu new_lv=%d\n", __func__,
                vclk->name, is_g3d, old_lv, fvmap_header[i].num_of_lv);

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
        pr_info("%s: margin_id=%d margin=%d\n", __func__, vclk->margin_id,
                margin);

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

            pr_info("%s: member=%d addr=0x%x lut_id=%d\n", __func__, j,
                    member_addr, vclk->list[j]);
        }

        // patch levels
        if (!strcmp(vclk->name, "dvfs_g3d")) {

            for (j = 0; j < fvmap_header[i].num_of_lv; j++) {
                new->table[j].rate = g3d_manual_ratevolt[j].rate;
                new->table[j].volt = g3d_manual_ratevolt[j].volt;
                pr_info("%s: g3d lv=%d rate=%u volt=%u\n", __func__, j,
                        new->table[j].rate, new->table[j].volt);
                pr_info("  lv g3d : [%7d], volt = %d uV (%d %%) \n",
                        new->table[j].rate, new->table[j].volt,
                        volt_offset_percent);
            }

        } else {
            for (j = 0; j < fvmap_header[i].num_of_lv; j++) {
                new->table[j].rate = old->table[j].rate;
                new->table[j].volt = old->table[j].volt;
                pr_info("%s: lv=%d rate=%u volt=%u\n", __func__, j,
                        new->table[j].rate, new->table[j].volt);
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

                pr_info("%s: lv=%d member=%d param_idx=%u value=%d\n", __func__,
                        j, k, param_idx, new_param->val[param_idx]);

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

    pr_info("%s: sram_base=%p map_base=%p size=%lu\n", __func__, sram_base,
            map_base, FVMAP_SIZE);

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
