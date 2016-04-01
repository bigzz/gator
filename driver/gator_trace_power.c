/**
 * Copyright (C) ARM Limited 2011-2015. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/cpufreq.h>
#include <trace/events/power.h>
#include <linux/workqueue.h>

#if defined(__arm__)

#include <asm/mach-types.h>

#define implements_wfi() (!machine_is_omap3_beagle())

#else

#define implements_wfi() false

#endif

/* cpu_frequency and cpu_idle trace points were introduced in Linux
 * kernel v2.6.38 the now deprecated power_frequency trace point was
 * available prior to 2.6.38, but only for x86
 */
#if GATOR_CPU_FREQ_SUPPORT
enum {
	POWER_CPU_FREQ,
	POWER_TOTAL
};

static DEFINE_PER_CPU(ulong, idle_prev_state);
static ulong power_cpu_enabled[POWER_TOTAL];
static ulong power_cpu_key[POWER_TOTAL];
static ulong power_gpu_enabled;
static ulong power_gpu_key;
static ulong power_cpu_cores;

static int gator_trace_power_create_files(struct super_block *sb, struct dentry *root)
{
	struct dentry *dir;
	int cpu;
	bool found_nonzero_freq = false;

	/* Even if CONFIG_CPU_FREQ is defined, it still may not be
	 * used. Check for non-zero values from cpufreq_quick_get
	 */
	for_each_online_cpu(cpu) {
		if (cpufreq_quick_get(cpu) > 0) {
			found_nonzero_freq = true;
			break;
		}
	}

	if (found_nonzero_freq) {
		/* cpu_frequency */
		dir = gatorfs_mkdir(sb, root, "Linux_power_cpu_freq");
		if (!dir)
			return -1;
		gatorfs_create_ulong(sb, dir, "enabled", &power_cpu_enabled[POWER_CPU_FREQ]);
		gatorfs_create_ro_ulong(sb, dir, "key", &power_cpu_key[POWER_CPU_FREQ]);
	}

	/* gpu_frequency */
	dir = gatorfs_mkdir(sb, root, "Linux_power_gpu_freq");
	if (!dir)
		return -1;
	gatorfs_create_ulong(sb, dir, "enabled", &power_gpu_enabled);
	gatorfs_create_ro_ulong(sb, dir, "key", &power_gpu_key);

	return 0;
}

/* 'cpu' may not equal smp_processor_id(), i.e. may not be running on the core that is having the freq/idle state change */
GATOR_DEFINE_PROBE(cpu_frequency, TP_PROTO(unsigned int frequency, unsigned int cpu))
{
	cpu = lcpu_to_pcpu(cpu);
	marshal_event_single64(cpu, power_cpu_key[POWER_CPU_FREQ], frequency * 1000L);
}

GATOR_DEFINE_PROBE(gpu_frequency, TP_PROTO(unsigned int frequency))
{
	int pcpu = get_physical_cpu();
	marshal_event_single64(pcpu, power_gpu_key, frequency * 1000L);
}

GATOR_DEFINE_PROBE(cpu_idle, TP_PROTO(unsigned int state, unsigned int cpu))
{
	cpu = lcpu_to_pcpu(cpu);

	if (state == per_cpu(idle_prev_state, cpu))
		return;

	if (implements_wfi())
		marshal_idle(cpu, state);

	per_cpu(idle_prev_state, cpu) = state;
}

static void gator_trace_power_online(void)
{
	int pcpu = get_physical_cpu();
	int lcpu = get_logical_cpu();

	if (power_cpu_enabled[POWER_CPU_FREQ])
		marshal_event_single64(pcpu, power_cpu_key[POWER_CPU_FREQ], cpufreq_quick_get(lcpu) * 1000L);
}

static void gator_trace_power_offline(void)
{
	/* Set frequency to zero on an offline */
	int cpu = get_physical_cpu();

	if (power_cpu_enabled[POWER_CPU_FREQ])
		marshal_event_single(cpu, power_cpu_key[POWER_CPU_FREQ], 0);
}

static void wq_freq_handler(struct work_struct *ignore);
static DECLARE_DELAYED_WORK(freq_work, wq_freq_handler);
static struct timer_list freq_wake_up_timer;
static void freq_wake_up_handler(unsigned long unused_data);

static void wq_freq_handler(struct work_struct *ignore)
{
	int cpu;
	struct cpufreq_policy *policy;
	for_each_present_cpu(cpu) {
		if(cpu_online(cpu)){
			policy = cpufreq_cpu_get(cpu);
			if (policy){
				marshal_event_single64(cpu, power_cpu_key[POWER_CPU_FREQ], policy->cur * 1000L);
				cpufreq_cpu_put(policy);
			}
		}else{
			marshal_event_single(cpu, power_cpu_key[POWER_CPU_FREQ], 0);
		}
	}
	mod_timer(&freq_wake_up_timer, jiffies + msecs_to_jiffies(500));
}

static void freq_wake_up_handler(unsigned long unused_data)
{
	schedule_delayed_work(&freq_work, 0);
}

static int gator_trace_power_start(void)
{
	int cpu;

	/* register tracepoints */
	if (power_cpu_enabled[POWER_CPU_FREQ])
		if (GATOR_REGISTER_TRACE(cpu_frequency))
			goto fail_cpu_frequency_exit;

	if (power_gpu_enabled)
		if (GATOR_REGISTER_TRACE(gpu_frequency))
			goto fail_gpufreq_set_exit;

	/* Always register for cpu_idle for detecting WFI */
	if (GATOR_REGISTER_TRACE(cpu_idle))
		goto fail_cpu_idle_exit;
	pr_debug("gator: registered power event tracepoints\n");

	schedule_delayed_work(&freq_work, 0);
	setup_deferrable_timer_on_stack(&freq_wake_up_timer, freq_wake_up_handler, 0);

	for_each_present_cpu(cpu) {
		per_cpu(idle_prev_state, cpu) = 0;
	}

	return 0;

	/* unregister tracepoints on error */
fail_cpu_idle_exit:
	if (power_gpu_enabled)
		GATOR_UNREGISTER_TRACE(gpu_frequency);
fail_gpufreq_set_exit:
	if (power_cpu_enabled[POWER_CPU_FREQ])
		GATOR_UNREGISTER_TRACE(cpu_frequency);
fail_cpu_frequency_exit:
	pr_err("gator: power event tracepoints failed to activate, please verify that tracepoints are enabled in the linux kernel\n");

	return -1;
}

static void gator_trace_power_stop(void)
{
	int i;

	if (power_cpu_enabled[POWER_CPU_FREQ])
		GATOR_UNREGISTER_TRACE(cpu_frequency);

	if (power_gpu_enabled)
		GATOR_UNREGISTER_TRACE(gpu_frequency);

	GATOR_UNREGISTER_TRACE(cpu_idle);
	pr_debug("gator: unregistered power event tracepoints\n");

	del_timer_sync(&freq_wake_up_timer);

	for (i = 0; i < POWER_TOTAL; i++)
		power_cpu_enabled[i] = 0;
}

static void gator_trace_power_init(void)
{
	int i;

	power_cpu_cores = nr_cpu_ids;
	for (i = 0; i < POWER_TOTAL; i++) {
		power_cpu_enabled[i] = 0;
		power_cpu_key[i] = gator_events_get_key();
	}
}
#else
static int gator_trace_power_create_files(struct super_block *sb, struct dentry *root)
{
	return 0;
}

static void gator_trace_power_online(void)
{
}

static void gator_trace_power_offline(void)
{
}

static int gator_trace_power_start(void)
{
	return 0;
}

static void gator_trace_power_stop(void)
{
}

static void gator_trace_power_init(void)
{
}
#endif
