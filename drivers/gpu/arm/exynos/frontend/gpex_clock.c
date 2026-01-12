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

#include <linux/slab.h>

#include <gpex_clock.h>
#include <gpex_qos.h>
#include <gpex_pm.h>
#include <gpex_dvfs.h>
#include <gpex_ifpo.h>
#include <gpex_utils.h>
#include <gpexbe_devicetree.h>
#include <gpexbe_clock.h>
#include <gpexbe_utilization.h>
#include <gpexbe_debug.h>

#include "gpex_clock_internal.h"

#define CPU_MAX INT_MAX

static struct _clock_info clk_info;

static void gpex_clock_log_table_state(const char *tag)
{
	(void)tag;
}

int gpex_clock_get_boot_clock(void)
{
	return clk_info.boot_clock;
}

int gpex_clock_get_max_clock(void)
{
	return clk_info.gpu_max_clock;
}

int gpex_clock_get_max_clock_limit(void)
{
	return clk_info.gpu_max_clock_limit;
}

int gpex_clock_get_min_clock(void)
{
	return clk_info.gpu_min_clock;
}

int gpex_clock_get_cur_clock(void)
{
	return clk_info.cur_clock;
}

int gpex_clock_get_max_lock(void)
{
	return clk_info.max_lock;
}

int gpex_clock_get_min_lock(void)
{
	return clk_info.min_lock;
}

int gpex_clock_get_clock(int level)
{
	return clk_info.table[level].clock;
}

u64 gpex_clock_get_time_busy(int level)
{
	return clk_info.table[level].time_busy;
}

/*******************************************
 * static helper functions
 ******************************************/
static int gpex_clock_update_config_data_from_dt(void)
{
	int ret = 0;
	struct freq_volt *fv_array;
	int asv_lv_num;
	int i, j;
	gpu_clock_info temp;

	clk_info.gpu_max_clock = gpexbe_devicetree_get_int(gpu_max_clock);
	clk_info.gpu_min_clock = gpexbe_devicetree_get_int(gpu_min_clock);
	clk_info.boot_clock = gpexbe_clock_get_boot_freq();
	clk_info.gpu_max_clock_limit = gpexbe_clock_get_max_freq();

	/* TODO: rename the table_size variable to something more sensible like row_cnt */
	clk_info.table_size = gpexbe_devicetree_get_int(gpu_dvfs_table_size.row);
	clk_info.table = kcalloc(clk_info.table_size, sizeof(gpu_clock_info), GFP_KERNEL);

	asv_lv_num = gpexbe_clock_get_level_num();
	fv_array = kcalloc(asv_lv_num, sizeof(*fv_array), GFP_KERNEL);
	if (!fv_array)
		return -ENOMEM;

	ret = gpexbe_clock_get_rate_asv_table(fv_array, asv_lv_num);
	if (!ret) {
		/* Preserve original behavior (no logging here). */
	}

	for (i = 0; i < asv_lv_num; i++) {
		int cal_freq = fv_array[i].freq;
		int cal_vol = fv_array[i].volt;
		dt_clock_item *dt_clock_table = gpexbe_devicetree_get_clock_table();

		if (cal_freq <= clk_info.gpu_max_clock && cal_freq >= clk_info.gpu_min_clock) {
			for (j = 0; j < clk_info.table_size; j++) {
				if (cal_freq == dt_clock_table[j].clock) {
					clk_info.table[j].clock = cal_freq;
					clk_info.table[j].voltage = cal_vol;
				}
			}
		}
	}

	kfree(fv_array);

	if (clk_info.table_size > 1 &&
	    clk_info.table[0].clock < clk_info.table[clk_info.table_size - 1].clock) {
		for (i = 0; i < clk_info.table_size / 2; i++) {
			temp = clk_info.table[i];
			clk_info.table[i] = clk_info.table[clk_info.table_size - 1 - i];
			clk_info.table[clk_info.table_size - 1 - i] = temp;
		}
	}

	gpex_clock_log_table_state("gpex_clock_init_table");

	return 0;
}

static int set_clock_using_calapi(int clk)
{
	int ret = 0;

	gpex_pm_lock();

	if (!gpex_pm_get_status(false)) {
		ret = -1;
		goto err;
	}

	if (clk == clk_info.cur_clock) {
		gpex_pm_unlock();
		return 0;
	}

	gpexbe_debug_dbg_snapshot_freq_in(clk_info.cur_clock, clk);

	gpexbe_clock_set_rate(clk);

	gpexbe_debug_dbg_snapshot_freq_out(clk_info.cur_clock, clk);

	clk_info.cur_clock = gpexbe_clock_get_rate();

err:
	gpex_pm_unlock();
	return ret;
}

int gpex_get_valid_gpu_clock(int clock, bool is_round_up)
{
	int i, min_clock_idx, max_clock_idx;

	/* Zero Value for Unlock */
	if (clock == 0)
		return 0;

	min_clock_idx = gpex_clock_get_table_idx(gpex_clock_get_min_clock());
	max_clock_idx = gpex_clock_get_table_idx(gpex_clock_get_max_clock());

	if ((clock - gpex_clock_get_min_clock()) < 0)
		return clk_info.table[min_clock_idx].clock;

	if (is_round_up) {
		/*
		 * Round Up if min lock sequence
		 * ex) invalid input 472Mhz -> valid min lock 400Mhz?500Mhz? -> min lock = 500Mhz
		 */
		for (i = min_clock_idx; i >= max_clock_idx; i--) {
			if (clock - (int)clk_info.table[i].clock <= 0)
				return clk_info.table[i].clock;
		}
	} else {
		/*
		 * Round Down if max lock sequence.
		 * ex) invalid input 472Mhz -> valid max lock 400Mhz?500Mhz? -> max lock = 400Mhz
		 */
		for (i = max_clock_idx; i <= min_clock_idx; i++) {
			if (clock - (int)clk_info.table[i].clock >= 0)
				return clk_info.table[i].clock;
		}
	}

	return -1;
}

int gpex_clock_update_time_in_state(int clock)
{
	u64 current_time;
	int level = gpex_clock_get_table_idx(clock);

	if (clk_info.prev_time_in_state_time == 0)
		clk_info.prev_time_in_state_time = get_jiffies_64();

	current_time = get_jiffies_64();

	if (level >= gpex_clock_get_table_idx(clk_info.gpu_max_clock) &&
	    level <= gpex_clock_get_table_idx(clk_info.gpu_min_clock)) {
		clk_info.table[level].time += current_time - clk_info.prev_time_in_state_time;
		clk_info.table[level].time_busy +=
			(u64)((current_time - clk_info.prev_time_in_state_time) *
			      gpexbe_utilization_get_utilization());
	}

	clk_info.prev_time_in_state_time = current_time;
	return 0;
}

u64 gpex_clock_get_time_in_state_last_update(void)
{
	return clk_info.prev_time_in_state_time;
}

static int gpex_clock_set_helper(int clock)
{
	int ret = 0;
	bool is_up = false;
	static int prev_clock = -1;
	int clk_idx = 0;

	if (gpex_ifpo_get_status() == IFPO_POWER_DOWN)
		return 0;

	clk_idx = gpex_clock_get_table_idx(clock);
	if (clk_idx < 0)
		return -1;

	is_up = prev_clock < clock;

	/*
	 * TODO: is there a need to set PMQOS before or after setting gpu clock?
	 * Why not move this call so pmqos is set in set_clock_using_calapi ?
	 */
	/* TODO: better range checking for platform step */
	if (is_up)
		gpex_qos_set_from_clock(clock);

	set_clock_using_calapi(clock);

	if (!is_up)
		gpex_qos_set_from_clock(clock);

	gpex_clock_update_time_in_state(prev_clock);
	prev_clock = clock;

	return ret;
}

int gpex_clock_init_time_in_state(void)
{
	int i;
	int max_clk_idx = gpex_clock_get_table_idx(clk_info.gpu_max_clock);
	int min_clk_idx = gpex_clock_get_table_idx(clk_info.gpu_min_clock);

	for (i = max_clk_idx; i <= min_clk_idx; i++) {
		clk_info.table[i].time = 0;
		clk_info.table[i].time_busy = 0;
	}

	return 0;
}

static int gpu_check_target_clock(int clock)
{
	int target_clock = clock;

	if (gpex_clock_get_table_idx(target_clock) < 0)
		return -1;

	if (!gpex_dvfs_get_status())
		return target_clock;

	if ((clk_info.min_lock > 0) && (gpex_pm_get_power_status()) &&
	    ((target_clock < clk_info.min_lock) || (clk_info.cur_clock < clk_info.min_lock)))
		target_clock = clk_info.min_lock;

	if ((clk_info.max_lock > 0) && (target_clock > clk_info.max_lock))
		target_clock = clk_info.max_lock;

	/* TODO: I don't think this required as it is set in gpex_dvfs_set_clock_callback */
	/* gpex_dvfs_set_step(gpex_clock_get_table_idx(target_clock)); */

	return target_clock;
}

/*******************************************************
 * Interfaced functions
 ******************************************************/
int gpex_clock_init(struct device **dev)
{
	int i = 0;

	mutex_init(&clk_info.clock_lock);
	clk_info.kbdev = container_of(dev, struct kbase_device, dev);
	clk_info.max_lock = 0;
	clk_info.min_lock = 0;

	for (i = 0; i < NUMBER_LOCK; i++) {
		clk_info.user_max_lock[i] = 0;
		clk_info.user_min_lock[i] = 0;
	}

	gpex_clock_update_config_data_from_dt();
	gpex_clock_init_time_in_state();
	gpex_clock_sysfs_init(&clk_info);

	gpex_utils_get_exynos_context()->clk_info = &clk_info;

	/* TODO: return proper error when error */
	return 0;
}

void gpex_clock_term(void)
{
	/* TODO: reset other clk_info variables too */
	clk_info.kbdev = NULL;
}

int gpex_clock_get_table_idx(int clock)
{
	int i;

	if (clock < clk_info.gpu_min_clock)
		return -1;

	for (i = 0; i < clk_info.table_size; i++) {
		if (clk_info.table[i].clock == clock)
			return i;
	}

	return -1;
}

int gpex_clock_get_clock_slow(void)
{
	return gpexbe_clock_get_rate();
}

int gpex_clock_set(int clk)
{
	int ret = 0, target_clk = 0;
	int prev_clk = 0;

	if (!gpex_pm_get_status(true))
		return -1;

	mutex_lock(&clk_info.clock_lock);

	if (!gpex_pm_get_power_status()) {
		mutex_unlock(&clk_info.clock_lock);
		return 0;
	}

	target_clk = gpu_check_target_clock(clk);
	if (target_clk < 0) {
		mutex_unlock(&clk_info.clock_lock);
		return -1;
	}

	gpex_pm_lock();

	if (gpex_pm_get_status(false))
		prev_clk = gpex_clock_get_clock_slow();

	gpex_pm_unlock();

	/* QOS is set here */
	gpex_clock_set_helper(target_clk);

	ret = gpex_dvfs_set_clock_callback();

	mutex_unlock(&clk_info.clock_lock);

	(void)prev_clk;
	return ret;
}

int gpex_clock_prepare_runtime_off(void)
{
	gpex_clock_update_time_in_state(clk_info.cur_clock);
	return 0;
}

int gpex_clock_lock_clock(gpex_clock_lock_cmd_t lock_command,
			  gpex_clock_lock_type_t lock_type,
			  int clock)
{
	int i;
	bool dirty = false;
	unsigned long flags;
	int max_lock_clk = 0;
	int valid_clock = 0;

	/* TODO: there's no need to check dvfs status anymore since dvfs and clock setting is separate */
	/* if (!gpex_dvfs_get_status()) */
	/*	return 0; */

	if (lock_type < TMU_LOCK || lock_type >= NUMBER_LOCK)
		return -1;

	valid_clock = clock;

	switch (lock_command) {
	case GPU_CLOCK_MAX_LOCK:
		gpex_dvfs_spin_lock(&flags);

		if (gpex_clock_get_table_idx(clock) < 0) {
			valid_clock = gpex_get_valid_gpu_clock(clock, false);
			if (valid_clock < 0) {
				gpex_dvfs_spin_unlock(&flags);
				return -1;
			}
		}

		clk_info.user_max_lock[lock_type] = valid_clock;
		clk_info.max_lock = valid_clock;

		if (clk_info.max_lock > 0) {
			for (i = 0; i < NUMBER_LOCK; i++) {
				if (clk_info.user_max_lock[i] > 0)
					clk_info.max_lock =
						MIN(clk_info.max_lock, clk_info.user_max_lock[i]);
			}
		} else {
			clk_info.max_lock = valid_clock;
		}

		gpex_dvfs_spin_unlock(&flags);

		if ((clk_info.max_lock > 0) && (gpex_clock_get_cur_clock() >= clk_info.max_lock))
			gpex_clock_set(clk_info.max_lock);

		break;

	case GPU_CLOCK_MIN_LOCK:
		gpex_dvfs_spin_lock(&flags);

		if (gpex_clock_get_table_idx(clock) < 0) {
			valid_clock = gpex_get_valid_gpu_clock(clock, true);
			if (valid_clock < 0) {
				gpex_dvfs_spin_unlock(&flags);
				return -1;
			}
		}

		clk_info.user_min_lock[lock_type] = valid_clock;
		clk_info.min_lock = valid_clock;

		if (clk_info.min_lock > 0) {
			for (i = 0; i < NUMBER_LOCK; i++) {
				if (clk_info.user_min_lock[i] > 0)
					clk_info.min_lock =
						MAX(clk_info.min_lock, clk_info.user_min_lock[i]);
			}
		} else {
			clk_info.min_lock = valid_clock;
		}

		max_lock_clk = (clk_info.max_lock == 0) ? gpex_clock_get_max_clock() : clk_info.max_lock;

		gpex_dvfs_spin_unlock(&flags);

		if ((clk_info.min_lock > 0) &&
		    (gpex_clock_get_cur_clock() < clk_info.min_lock) &&
		    (clk_info.min_lock <= max_lock_clk))
			gpex_clock_set(clk_info.min_lock);

		break;

	case GPU_CLOCK_MAX_UNLOCK:
		gpex_dvfs_spin_lock(&flags);

		clk_info.user_max_lock[lock_type] = 0;
		clk_info.max_lock = gpex_clock_get_max_clock();

		for (i = 0; i < NUMBER_LOCK; i++) {
			if (clk_info.user_max_lock[i] > 0) {
				dirty = true;
				clk_info.max_lock =
					MIN(clk_info.user_max_lock[i], clk_info.max_lock);
			}
		}

		if (!dirty)
			clk_info.max_lock = 0;

		gpex_dvfs_spin_unlock(&flags);
		break;

	case GPU_CLOCK_MIN_UNLOCK:
		gpex_dvfs_spin_lock(&flags);

		clk_info.user_min_lock[lock_type] = 0;
		clk_info.min_lock = gpex_clock_get_min_clock();

		for (i = 0; i < NUMBER_LOCK; i++) {
			if (clk_info.user_min_lock[i] > 0) {
				dirty = true;
				clk_info.min_lock =
					MAX(clk_info.user_min_lock[i], clk_info.min_lock);
			}
		}

		if (!dirty)
			clk_info.min_lock = 0;

		gpex_dvfs_spin_unlock(&flags);
		break;

	default:
		break;
	}

	return 0;
}

void gpex_clock_mutex_lock(void)
{
	mutex_lock(&clk_info.clock_lock);
}

void gpex_clock_mutex_unlock(void)
{
	mutex_unlock(&clk_info.clock_lock);
}

int gpex_clock_get_voltage(int clk)
{
	int idx = gpex_clock_get_table_idx(clk);

	if (idx >= 0 && idx < clk_info.table_size)
		return clk_info.table[idx].voltage;

	return -EINVAL;
}
