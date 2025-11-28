/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Central definition of GPU DVFS overrides for Exynos9820 based platforms.
 *
 * Add custom frequency/voltage pairs here and the helper routines in this
 * header will make sure they are propagated consistently across CAL, FVMap
 * and vclk consumers.
 */

#ifndef __GPU_DVFS_OVERRIDES_H__
#define __GPU_DVFS_OVERRIDES_H__

#include <linux/kernel.h>

#include "g3d_dvfs_table.h"

struct gpu_dvfs_override_entry {
	unsigned long rate_khz;
	unsigned int volt_uv;
};

#define G3D_OVERRIDE_ENTRY(rate_khz, volt_uv, pll_freq_hz, p, m, s, k, override) \
	G3D_OVERRIDE_ENTRY_##override(rate_khz, volt_uv)
#define G3D_OVERRIDE_ENTRY_1(rate_khz, volt_uv) { rate_khz, volt_uv },
#define G3D_OVERRIDE_ENTRY_0(rate_khz, volt_uv)

static const struct gpu_dvfs_override_entry gpu_dvfs_overrides[] = {
	G3D_DVFS_TABLE_ENTRY_LIST(G3D_OVERRIDE_ENTRY)
};

#undef G3D_OVERRIDE_ENTRY_0
#undef G3D_OVERRIDE_ENTRY_1
#undef G3D_OVERRIDE_ENTRY

static inline size_t gpu_dvfs_override_count(void)
{
	return ARRAY_SIZE(gpu_dvfs_overrides);
}

static inline bool gpu_dvfs_has_overrides(void)
{
	return gpu_dvfs_override_count() > 0;
}

static inline unsigned long gpu_dvfs_override_highest_rate(void)
{
	unsigned long rate = 0;
	size_t i;

	for (i = 0; i < gpu_dvfs_override_count(); i++) {
		rate = max(rate, gpu_dvfs_overrides[i].rate_khz);
	}

	return rate;
}

static inline const struct gpu_dvfs_override_entry *
gpu_dvfs_override_lookup(unsigned long rate_khz)
{
	size_t i;

	for (i = 0; i < gpu_dvfs_override_count(); i++) {
		if (gpu_dvfs_overrides[i].rate_khz == rate_khz)
			return &gpu_dvfs_overrides[i];
	}

	return NULL;
}

static inline const struct gpu_dvfs_override_entry *
gpu_dvfs_override_get(size_t index)
{
	if (index >= gpu_dvfs_override_count())
		return NULL;

	return &gpu_dvfs_overrides[index];
}

#endif /* __GPU_DVFS_OVERRIDES_H__ */
