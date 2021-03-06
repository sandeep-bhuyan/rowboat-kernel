/*
 * linux/arch/arm/mach-omap2/resource34xx.c
 * OMAP3 resource init/change_level/validate_level functions
 *
 * Copyright (C) 2007-2008 Texas Instruments, Inc.
 * Rajendra Nayak <rnayak@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 * History:
 *
 */

#include <linux/pm_qos_params.h>
#include <linux/cpufreq.h>
#include <mach/powerdomain.h>
#include <mach/clockdomain.h>
#include <mach/omap34xx.h>
#include "smartreflex.h"
#include "resource34xx.h"
#include "pm.h"



/**
 * init_latency - Initializes the mpu/core latency resource.
 * @resp: Latency resource to be initalized
 *
 * No return value.
 */
void init_latency(struct shared_resource *resp)
{
	resp->no_of_users = 0;
	resp->curr_level = RES_DEFAULTLEVEL;
	*((u8 *)resp->resource_data) = 0;
	return;
}

/**
 * set_latency - Adds/Updates and removes the CPU_DMA_LATENCY in *pm_qos_params.
 * @resp: resource pointer
 * @latency: target latency to be set
 *
 * Returns 0 on success, or error values as returned by
 * pm_qos_update_requirement/pm_qos_add_requirement.
 */
int set_latency(struct shared_resource *resp, u32 latency)
{
	u8 *pm_qos_req_added;

	if (resp->curr_level == latency)
		return 0;
	else
		/* Update the resources current level */
		resp->curr_level = latency;

	pm_qos_req_added = resp->resource_data;
	if (latency == RES_DEFAULTLEVEL)
		/* No more users left, remove the pm_qos_req if present */
		if (*pm_qos_req_added) {
			pm_qos_remove_requirement(PM_QOS_CPU_DMA_LATENCY,
							resp->name);
			*pm_qos_req_added = 0;
			return 0;
		}

	if (*pm_qos_req_added) {
		return pm_qos_update_requirement(PM_QOS_CPU_DMA_LATENCY,
						resp->name, latency);
	} else {
		*pm_qos_req_added = 1;
		return pm_qos_add_requirement(PM_QOS_CPU_DMA_LATENCY,
						resp->name, latency);
	}
}

/**
 * init_pd_latency - Initializes the power domain latency resource.
 * @resp: Power Domain Latency resource to be initialized.
 *
 * No return value.
 */
void init_pd_latency(struct shared_resource *resp)
{
	struct pd_latency_db *pd_lat_db;

	resp->no_of_users = 0;
	if (enable_off_mode)
		resp->curr_level = PD_LATENCY_OFF;
	else
		resp->curr_level = PD_LATENCY_RET;
	pd_lat_db = resp->resource_data;
	/* Populate the power domain associated with the latency resource */
	pd_lat_db->pd = pwrdm_lookup(pd_lat_db->pwrdm_name);
	set_pwrdm_state(pd_lat_db->pd, resp->curr_level);
	return;
}

/**
 * set_pd_latency - Updates the curr_level of the power domain resource.
 * @resp: Power domain latency resource.
 * @latency: New latency value acceptable.
 *
 * This function maps the latency in microsecs to the acceptable
 * Power domain state using the latency DB.
 * It then programs the power domain to enter the target state.
 * Always returns 0.
 */
int set_pd_latency(struct shared_resource *resp, u32 latency)
{
	u32 pd_lat_level, ind;
	struct pd_latency_db *pd_lat_db;
	struct powerdomain *pwrdm;

	pd_lat_db = resp->resource_data;
	pwrdm = pd_lat_db->pd;
	pd_lat_level = PD_LATENCY_OFF;
	/* using the latency db map to the appropriate PD state */
	for (ind = 0; ind < PD_LATENCY_MAXLEVEL; ind++) {
		if (pd_lat_db->latency[ind] < latency) {
			pd_lat_level = ind;
			break;
		}
	}

	if (!enable_off_mode && pd_lat_level == PD_LATENCY_OFF)
		pd_lat_level = PD_LATENCY_RET;

	resp->curr_level = pd_lat_level;
	set_pwrdm_state(pwrdm, pd_lat_level);
	return 0;
}

static struct clk *vdd1_clk;
static struct clk *vdd2_clk;
static struct shared_resource *vdd1_resp;
static struct shared_resource *vdd2_resp;
static struct device dummy_mpu_dev;
static struct device dummy_dsp_dev;
static int vdd1_lock;
static int vdd2_lock;

/**
 * init_opp - Initialize the OPP resource
 */
void init_opp(struct shared_resource *resp)
{
	resp->no_of_users = 0;

	if (!mpu_opps || !dsp_opps || !l3_opps)
		return;

	/* Initialize the current level of the OPP resource
	* to the  opp set by u-boot.
	*/
	if (strcmp(resp->name, "vdd1_opp") == 0) {
		resp->curr_level = curr_vdd1_prcm_set->opp_id;
		vdd1_clk = clk_get(NULL, "virt_vdd1_prcm_set");
		vdd1_resp = resp;
	} else if (strcmp(resp->name, "vdd2_opp") == 0) {
		resp->curr_level = curr_vdd2_prcm_set->opp_id;
		vdd2_clk = clk_get(NULL, "virt_vdd2_prcm_set");
		vdd2_resp = resp;
	}
	return;
}

static struct device vdd2_dev;

int resource_access_opp_lock(int res, int delta)
{
	if (res == VDD1_OPP) {
		vdd1_lock += delta;
		return vdd1_lock;
	} else if (res == VDD2_OPP) {
		vdd2_lock += delta;
		return vdd2_lock;
	}
	return -EINVAL;
}

int resource_set_opp_level(int res, u32 target_level, int flags)
{
	unsigned long mpu_freq, mpu_old_freq, l3_freq, t_opp;
#ifdef CONFIG_CPU_FREQ
	struct cpufreq_freqs freqs_notify;
#endif
	struct shared_resource *resp;
	int ret;

	if (res == VDD1_OPP)
		resp = vdd1_resp;
	else if (res == VDD2_OPP)
		resp = vdd2_resp;
	else
		return 0;

	if (resp->curr_level == target_level)
		return 0;

	if (!mpu_opps || !dsp_opps || !l3_opps)
		return 0;

	if (res == VDD1_OPP) {
		if (flags != OPP_IGNORE_LOCK && vdd1_lock)
			return 0;
		mpu_old_freq = get_freq(mpu_opps + MAX_VDD1_OPP,
					curr_vdd1_prcm_set->opp_id);
		mpu_freq = get_freq(mpu_opps + MAX_VDD1_OPP,
					target_level);
#ifdef CONFIG_CPU_FREQ
		freqs_notify.old = mpu_old_freq/1000;
		freqs_notify.new = mpu_freq/1000;
		freqs_notify.cpu = 0;
		/* Send pre notification to CPUFreq */
		cpufreq_notify_transition(&freqs_notify, CPUFREQ_PRECHANGE);
#endif
		t_opp = ID_VDD(PRCM_VDD1) |
			ID_OPP_NO(mpu_opps[target_level].opp_id);

		/* For VDD1 OPP3 and above, make sure the interconnect
		 * is at 100Mhz or above.
		 * throughput in KiB/s for 100 Mhz = 100 * 1000 * 4.
		 */
		if (mpu_opps[target_level].opp_id >= 3)
			resource_request("vdd2_opp", &vdd2_dev, 400000);

		if (resp->curr_level > target_level) {
			/* Scale Frequency and then voltage */
			clk_set_rate(vdd1_clk, mpu_freq);
#ifdef CONFIG_OMAP_SMARTREFLEX
			sr_voltagescale_vcbypass(t_opp,
					mpu_opps[target_level].vsel);
#endif
		} else {
#ifdef CONFIG_OMAP_SMARTREFLEX
			/* Scale Voltage and then frequency */
			sr_voltagescale_vcbypass(t_opp,
					mpu_opps[target_level].vsel);
#endif
			clk_set_rate(vdd1_clk, mpu_freq);
		}

		/* Release the VDD2/interconnect constraint */
		if (mpu_opps[target_level].opp_id < 3)
			resource_release("vdd2_opp", &vdd2_dev);

		resp->curr_level = curr_vdd1_prcm_set->opp_id;
#ifdef CONFIG_CPU_FREQ
		/* Send a post notification to CPUFreq */
		cpufreq_notify_transition(&freqs_notify, CPUFREQ_POSTCHANGE);
#endif
	} else {
		if (flags != OPP_IGNORE_LOCK && vdd2_lock)
			return 0;
		l3_freq = get_freq(l3_opps + MAX_VDD2_OPP,
					target_level);
		t_opp = ID_VDD(PRCM_VDD2) |
			ID_OPP_NO(l3_opps[target_level].opp_id);
		if (resp->curr_level > target_level) {
			/* Scale Frequency and then voltage */
			ret = clk_set_rate(vdd2_clk, l3_freq);
			if (ret)
				return ret;
#ifdef CONFIG_OMAP_SMARTREFLEX
			sr_voltagescale_vcbypass(t_opp,
					l3_opps[target_level].vsel);
#endif
		} else {
#ifdef CONFIG_OMAP_SMARTREFLEX
			/* Scale Voltage and then frequency */
			sr_voltagescale_vcbypass(t_opp,
					l3_opps[target_level].vsel);
#endif
			ret = clk_set_rate(vdd2_clk, l3_freq);
			if (ret) {
#ifdef CONFIG_OMAP_SMARTREFLEX
				/* Setting clock failed, revert voltage */
				t_opp = ID_VDD(PRCM_VDD2) |
					ID_OPP_NO(l3_opps[resp->curr_level].
							opp_id);
				sr_voltagescale_vcbypass(t_opp,
					l3_opps[resp->curr_level].vsel);
#endif
				return ret;
			}
		}
		resp->curr_level = curr_vdd2_prcm_set->opp_id;
	}
	return 0;
}

int set_opp(struct shared_resource *resp, u32 target_level)
{
	unsigned long tput;
	unsigned long req_l3_freq;
	int ind;

	if (resp == vdd1_resp) {
		resource_set_opp_level(VDD1_OPP, target_level, 0);
	} else if (resp == vdd2_resp) {
		tput = target_level;

		/* Convert the tput in KiB/s to Bus frequency in MHz */
		req_l3_freq = (tput * 1000)/4;

		for (ind = 2; ind <= MAX_VDD2_OPP; ind++)
			if ((l3_opps + ind)->rate >= req_l3_freq) {
				target_level = ind;
				break;
			}

		/* Set the highest OPP possible */
		if (ind > MAX_VDD2_OPP)
			target_level = ind-1;
		resource_set_opp_level(VDD2_OPP, target_level, 0);
	}
	return 0;
}

/**
 * validate_opp - Validates if valid VDD1 OPP's are passed as the
 * target_level.
 * VDD2 OPP levels are passed as L3 throughput, which are then mapped
 * to an appropriate OPP.
 */
int validate_opp(struct shared_resource *resp, u32 target_level)
{
	return 0;
}

/**
 * init_freq - Initialize the frequency resource.
 */
void init_freq(struct shared_resource *resp)
{
	char *linked_res_name;
	resp->no_of_users = 0;

	if (!mpu_opps || !dsp_opps)
		return;

	linked_res_name = (char *)resp->resource_data;
	/* Initialize the current level of the Freq resource
	* to the frequency set by u-boot.
	*/
	if (strcmp(resp->name, "mpu_freq") == 0)
		/* MPU freq in Mhz */
		resp->curr_level = curr_vdd1_prcm_set->rate;
	else if (strcmp(resp->name, "dsp_freq") == 0)
		/* DSP freq in Mhz */
		resp->curr_level = get_freq(dsp_opps + MAX_VDD2_OPP,
						curr_vdd1_prcm_set->opp_id);
	return;
}

int set_freq(struct shared_resource *resp, u32 target_level)
{
	unsigned int vdd1_opp;

	if (!mpu_opps || !dsp_opps)
		return 0;

	if (strcmp(resp->name, "mpu_freq") == 0) {
		vdd1_opp = get_opp(mpu_opps + MAX_VDD1_OPP, target_level);
		resource_request("vdd1_opp", &dummy_mpu_dev, vdd1_opp);
	} else if (strcmp(resp->name, "dsp_freq") == 0) {
		vdd1_opp = get_opp(dsp_opps + MAX_VDD1_OPP, target_level);
		resource_request("vdd1_opp", &dummy_dsp_dev, vdd1_opp);
	}
	resp->curr_level = target_level;
	return 0;
}

int validate_freq(struct shared_resource *resp, u32 target_level)
{
	return 0;
}
