/* SPDX-License-Identifier: GPL-2.0 */

/*
 * (C) COPYRIGHT 2021 Samsung Electronics Inc. All rights reserved.
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU licence.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can access it online at
 * http://www.gnu.org/licenses/gpl-2.0.html.
 */

#include <linux/printk.h>
#include <soc/samsung/cal-if.h>

#include <gpexbe_devicetree.h>

#include <gpexbe_clock.h>
#include <gpex_utils.h>
#include <gpex_debug.h>

struct _clock_backend_info {
	int boot_clock;
	int max_clock_limit;
};

static struct _clock_backend_info pm_info;
static unsigned int cal_id;

int gpexbe_clock_get_level_num(void)
{
	int levels = cal_dfs_get_lv_num(cal_id);

	pr_info("[gpexbe] cal_id %u reports %d DVFS levels\n", cal_id, levels);

	return levels;
}

int gpexbe_clock_get_rate_asv_table(struct freq_volt *fv_array, int level_num)
{
	int i;
	int ret = 0;
	struct dvfs_rate_volt rate_volt[48];

	ret = cal_dfs_get_rate_asv_table(cal_id, rate_volt);

	pr_info("[gpexbe] cal_id %u ASV table entries requested: %d\n",
		cal_id, level_num);

	if (!ret) {
		pr_info("[gpexbe] cal_id %u returned 0 ASV entries\n", cal_id);
		/* TODO: print error. Also remove this size limit by using dynamic alloc */
		return ret;
	}

	if (ret < level_num)
		pr_info("[gpexbe] cal_id %u only supplied %d entries (requested %d)\n",
			cal_id, ret, level_num);

	for (i = 0; i < level_num; i++) {
		fv_array[i].freq = rate_volt[i].rate;
		fv_array[i].volt = rate_volt[i].volt;
		pr_info("[gpexbe]   level %02d : %8d kHz @ %d uV\n", i,
			fv_array[i].freq, fv_array[i].volt);
	}

	return ret;
}

int gpexbe_clock_get_boot_freq(void)
{
	return pm_info.boot_clock;
}

int gpexbe_clock_get_max_freq(void)
{
	return pm_info.max_clock_limit;
}

int gpexbe_clock_set_rate(int clk)
{
	int ret = 0;

	gpex_debug_new_record(HIST_CLOCK);
	gpex_debug_record_prev_data(HIST_CLOCK, gpexbe_clock_get_rate());

	ret = cal_dfs_set_rate(cal_id, clk);

	gpex_debug_record_time(HIST_CLOCK);
	gpex_debug_record_code(HIST_CLOCK, ret);
	gpex_debug_record_new_data(HIST_CLOCK, clk);

	if (ret)
		gpex_debug_incr_error_cnt(HIST_CLOCK);

	return ret;
}

int gpexbe_clock_get_rate(void)
{
	return cal_dfs_get_rate(cal_id);
}

int gpexbe_clock_init(void)
{
	cal_id = gpexbe_devicetree_get_int(g3d_cmu_cal_id);

	if (!cal_id) {
		/* TODO: print error cal id not found */
		pr_info("[gpexbe] failed to read g3d_cmu_cal_id from DT\n");
		return -1;
	}

	pm_info.boot_clock = cal_dfs_get_boot_freq(cal_id);
	pm_info.max_clock_limit = (int)cal_dfs_get_max_freq(cal_id);

	pr_info("[gpexbe] init cal_id %u boot %d kHz limit %d kHz\n",
		cal_id, pm_info.boot_clock, pm_info.max_clock_limit);

	gpex_utils_get_exynos_context()->pm_info = &pm_info;

	return 0;
}

void gpexbe_clock_term(void)
{
	cal_id = 0;
	pm_info.boot_clock = 0;
	pm_info.max_clock_limit = 0;
}
