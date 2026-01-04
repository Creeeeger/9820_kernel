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

#include <gpex_dvfs.h>
#include <gpex_clock.h>
#include <gpex_pm.h>
#include <gpex_utils.h>
#include <gpex_clboost.h>
#include <gpex_tsg.h>

#include "gpex_dvfs_internal.h"
#include "gpu_dvfs_governor.h"

static struct dvfs_info *dvfs;

/* TODO: This should be moved to DVFS module */
int gpex_dvfs_set_clock_callback(void)
{
	unsigned long flags;
	int level = 0;

	int cur_clock = 0;
	cur_clock = gpex_clock_get_cur_clock();

	level = gpex_clock_get_table_idx(cur_clock);
	if (level >= 0) {
		spin_lock_irqsave(&dvfs->spinlock, flags);

		if (dvfs->step != level)
			dvfs->down_requirement = dvfs->table[level].down_staycount;

		if (dvfs->step < level)
			dvfs->interactive.delay_count = 0;

		dvfs->step = level;
		//gpex_dvfs_set_step(level);

		spin_unlock_irqrestore(&dvfs->spinlock, flags);
	} else {
		GPU_LOG(MALI_EXYNOS_ERROR, "%s: invalid dvfs level returned %d gpu power %d\n",
			__func__, cur_clock, gpex_pm_get_status(false));
		return -1;
	}
	return 0;
}

typedef int (*GET_NEXT_LEVEL)(int utilization);
static GET_NEXT_LEVEL gpu_dvfs_get_next_level;

static int gpu_dvfs_governor_interactive(int utilization);
static int gpu_dvfs_governor_booster(int utilization);

static gpu_dvfs_governor_info governor_info[G3D_MAX_GOVERNOR_NUM] = {
	{
		G3D_DVFS_GOVERNOR_INTERACTIVE,
		"Interactive",
		gpu_dvfs_governor_interactive,
	},
	{
		G3D_DVFS_GOVERNOR_BOOSTER,
		"Booster",
		gpu_dvfs_governor_booster,
	},
};

void gpu_dvfs_update_start_clk(int governor_type, int clk)
{
	governor_info[governor_type].start_clk = clk;
}

void *gpu_dvfs_get_governor_info(void)
{
	return &governor_info;
}

static int gpu_dvfs_governor_interactive(int utilization)
{
	if ((dvfs->step > gpex_clock_get_table_idx(gpex_clock_get_max_clock())) &&
	    (utilization > dvfs->table[dvfs->step].max_threshold)) {
		int highspeed_level = gpex_clock_get_table_idx(dvfs->interactive.highspeed_clock);
		if ((highspeed_level > 0) && (dvfs->step > highspeed_level) &&
		    (utilization > dvfs->interactive.highspeed_load)) {
			if (dvfs->interactive.delay_count == dvfs->interactive.highspeed_delay) {
				dvfs->step = highspeed_level;
				dvfs->interactive.delay_count = 0;
			} else {
				dvfs->interactive.delay_count++;
			}
		} else {
			dvfs->step--;
			dvfs->interactive.delay_count = 0;
		}
		if (dvfs->table[dvfs->step].clock > gpex_clock_get_max_clock_limit())
			dvfs->step = gpex_clock_get_table_idx(gpex_clock_get_max_clock_limit());
		dvfs->down_requirement = dvfs->table[dvfs->step].down_staycount;
	} else if ((dvfs->step < gpex_clock_get_table_idx(gpex_clock_get_min_clock())) &&
		   (utilization < dvfs->table[dvfs->step].min_threshold)) {
		dvfs->interactive.delay_count = 0;
		dvfs->down_requirement--;
		if (dvfs->down_requirement == 0) {
			dvfs->step++;
			dvfs->down_requirement = dvfs->table[dvfs->step].down_staycount;
		}
	} else {
		dvfs->interactive.delay_count = 0;
		dvfs->down_requirement = dvfs->table[dvfs->step].down_staycount;
	}

	DVFS_ASSERT(dvfs->step <= gpex_clock_get_table_idx(gpex_clock_get_min_clock()));

	return 0;
}

static int gpu_dvfs_governor_booster(int utilization)
{
	static int weight;
	int cur_weight, booster_threshold, dvfs_table_lock;

	cur_weight = gpex_clock_get_cur_clock() * utilization;
	/* booster_threshold = current clock * set the percentage of utilization */
	booster_threshold = gpex_clock_get_cur_clock() * 50;

	dvfs_table_lock = gpex_clock_get_table_idx(gpex_clock_get_max_clock());

	if ((dvfs->step >= dvfs_table_lock + 2) && ((cur_weight - weight) > booster_threshold)) {
		dvfs->step -= 2;
		dvfs->down_requirement = dvfs->table[dvfs->step].down_staycount;
		GPU_LOG(MALI_EXYNOS_WARNING, "Booster Governor: G3D level 2 step\n");
	} else if ((dvfs->step > gpex_clock_get_table_idx(gpex_clock_get_max_clock())) &&
		   (utilization > dvfs->table[dvfs->step].max_threshold)) {
		dvfs->step--;
		dvfs->down_requirement = dvfs->table[dvfs->step].down_staycount;
	} else if ((dvfs->step < gpex_clock_get_table_idx(gpex_clock_get_min_clock())) &&
		   (utilization < dvfs->table[dvfs->step].min_threshold)) {
		dvfs->down_requirement--;
		if (dvfs->down_requirement == 0) {
			dvfs->step++;
			dvfs->down_requirement = dvfs->table[dvfs->step].down_staycount;
		}
	} else {
		dvfs->down_requirement = dvfs->table[dvfs->step].down_staycount;
	}

	DVFS_ASSERT((dvfs->step >= gpex_clock_get_table_idx(gpex_clock_get_max_clock())) &&
		    (dvfs->step <= gpex_clock_get_table_idx(gpex_clock_get_min_clock())));

	weight = cur_weight;

	return 0;
}

int gpu_dvfs_decide_next_freq(int utilization)
{
	unsigned long flags;

	if (gpex_tsg_get_migov_mode() == 1 && gpex_tsg_get_is_gov_set() != 1) {
		gpu_dvfs_governor_setting_locked(G3D_DVFS_GOVERNOR_INTERACTIVE);
		gpex_tsg_set_saved_polling_speed(gpex_dvfs_get_polling_speed());
		gpex_dvfs_set_polling_speed(16);
		gpex_tsg_set_is_gov_set(1);
		gpex_tsg_set_en_signal(false);
		gpex_tsg_set_pmqos(true);
	} else if (gpex_tsg_get_migov_mode() != 1 && gpex_tsg_get_is_gov_set() != 0) {
		gpu_dvfs_governor_setting_locked(gpex_tsg_get_governor_type_init());
		gpex_dvfs_set_polling_speed(gpex_tsg_get_saved_polling_speed());
		gpex_tsg_set_is_gov_set(0);
		gpex_tsg_set_pmqos(false);
		gpex_tsg_reset_acc_count();
	}

	gpex_tsg_input_nr_acc_cnt();

	spin_lock_irqsave(&dvfs->spinlock, flags);
	gpu_dvfs_get_next_level(utilization);
	spin_unlock_irqrestore(&dvfs->spinlock, flags);

	if (gpex_clboost_check_activation_condition())
		dvfs->step = gpex_clock_get_table_idx(gpex_clock_get_max_clock());

	return dvfs->table[dvfs->step].clock;
}

int gpu_dvfs_governor_setting(int governor_type)
{
	unsigned long flags;

	if ((governor_type < 0) || (governor_type >= G3D_MAX_GOVERNOR_NUM)) {
		GPU_LOG(MALI_EXYNOS_WARNING, "%s: invalid governor type (%d)\n", __func__,
			governor_type);
		return -1;
	}

	spin_lock_irqsave(&dvfs->spinlock, flags);
	dvfs->step = gpex_clock_get_table_idx(governor_info[governor_type].start_clk);
	gpu_dvfs_get_next_level = (GET_NEXT_LEVEL)(governor_info[governor_type].governor);

	dvfs->env_data.utilization = 80;

	dvfs->down_requirement = 1;
	dvfs->governor_type = governor_type;

	/* TODO: why set the cur_clock here? cur_clock should be set when the actual clock is changed */
	//gpex_clock_get_cur_clock() = dvfs->table[dvfs->step].clock;

	spin_unlock_irqrestore(&dvfs->spinlock, flags);

	return 0;
}

int gpu_dvfs_governor_setting_locked(int governor_type)
{
	unsigned long flags;

	if ((governor_type < 0) || (governor_type >= G3D_MAX_GOVERNOR_NUM)) {
		GPU_LOG(MALI_EXYNOS_WARNING, "%s: invalid governor type (%d)\n", __func__,
			governor_type);
		return -1;
	}

	spin_lock_irqsave(&dvfs->spinlock, flags);
	dvfs->step = gpex_clock_get_table_idx(governor_info[governor_type].start_clk);
	gpu_dvfs_get_next_level = (GET_NEXT_LEVEL)(governor_info[governor_type].governor);

	dvfs->governor_type = governor_type;

	spin_unlock_irqrestore(&dvfs->spinlock, flags);

	return 0;
}

int gpu_dvfs_governor_init(struct dvfs_info *_dvfs)
{
	int governor_type = G3D_DVFS_GOVERNOR_INTERACTIVE;

	dvfs = _dvfs;

	governor_type = dvfs->governor_type;

	if (gpu_dvfs_governor_setting(governor_type) < 0) {
		GPU_LOG(MALI_EXYNOS_WARNING, "%s: fail to initialize governor\n", __func__);
		return -1;
	}

	return 0;
}
