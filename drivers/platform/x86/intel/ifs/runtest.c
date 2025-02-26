// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2022 Intel Corporation. */

#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/nmi.h>
#include <linux/slab.h>
#include <linux/stop_machine.h>

#include "ifs.h"

/*
 * Note all code and data in this file is protected by
 * ifs_sem. On HT systems all threads on a core will
 * execute together, but only the first thread on the
 * core will update results of the test.
 */

#define CREATE_TRACE_POINTS
#include <trace/events/intel_ifs.h>

/* Max retries on the same chunk */
#define MAX_IFS_RETRIES  5

struct run_params {
	struct ifs_data *ifsd;
	union ifs_scan *activate;
	union ifs_status status;
};

struct run_array_params {
	struct ifs_data *ifsd;
	union ifs_array *command;
};

/*
 * Number of TSC cycles that a logical CPU will wait for the other
 * logical CPU on the core in the WRMSR(ACTIVATE_SCAN).
 */
#define IFS_THREAD_WAIT 100000

enum ifs_status_err_code {
	IFS_NO_ERROR				= 0,
	IFS_OTHER_THREAD_COULD_NOT_JOIN		= 1,
	IFS_INTERRUPTED_BEFORE_RENDEZVOUS	= 2,
	IFS_POWER_MGMT_INADEQUATE_FOR_SCAN	= 3,
	IFS_INVALID_CHUNK_RANGE			= 4,
	IFS_MISMATCH_ARGUMENTS_BETWEEN_THREADS	= 5,
	IFS_CORE_NOT_CAPABLE_CURRENTLY		= 6,
	IFS_UNASSIGNED_ERROR_CODE		= 7,
	IFS_EXCEED_NUMBER_OF_THREADS_CONCURRENT	= 8,
	IFS_INTERRUPTED_DURING_EXECUTION	= 9,
	IFS_UNASSIGNED_ERROR_CODE_0xA		= 0xA,
	IFS_CORRUPTED_CHUNK		= 0xB,
};

static const char * const scan_test_status[] = {
	[IFS_NO_ERROR] = "SCAN no error",
	[IFS_OTHER_THREAD_COULD_NOT_JOIN] = "Other thread could not join.",
	[IFS_INTERRUPTED_BEFORE_RENDEZVOUS] = "Interrupt occurred prior to SCAN coordination.",
	[IFS_POWER_MGMT_INADEQUATE_FOR_SCAN] =
	"Core Abort SCAN Response due to power management condition.",
	[IFS_INVALID_CHUNK_RANGE] = "Non valid chunks in the range",
	[IFS_MISMATCH_ARGUMENTS_BETWEEN_THREADS] = "Mismatch in arguments between threads T0/T1.",
	[IFS_CORE_NOT_CAPABLE_CURRENTLY] = "Core not capable of performing SCAN currently",
	[IFS_UNASSIGNED_ERROR_CODE] = "Unassigned error code 0x7",
	[IFS_EXCEED_NUMBER_OF_THREADS_CONCURRENT] =
	"Exceeded number of Logical Processors (LP) allowed to run Scan-At-Field concurrently",
	[IFS_INTERRUPTED_DURING_EXECUTION] = "Interrupt occurred prior to SCAN start",
	[IFS_UNASSIGNED_ERROR_CODE_0xA] = "Unassigned error code 0xA",
	[IFS_CORRUPTED_CHUNK] = "Scan operation aborted due to corrupted image. Try reloading",
};

static void message_not_tested(struct device *dev, int cpu, union ifs_status status)
{
	struct ifs_data *ifsd = ifs_get_data(dev);

	/*
	 * control_error is set when the microcode runs into a problem
	 * loading the image from the reserved BIOS memory, or it has
	 * been corrupted. Reloading the image may fix this issue.
	 */
	if (status.control_error) {
		dev_warn(dev, "CPU %d: Scan controller error. Batch: %02x version: 0x%x\n", cpu,
			 ifsd->cur_batch, ifsd->loaded_version);
		return;
	}

	if (status.error_code < ARRAY_SIZE(scan_test_status)) {
		dev_info(dev, "CPU %d: SCAN operation did not start. %s\n", cpu,
			 scan_test_status[status.error_code]);
	} else if (status.error_code == IFS_SW_TIMEOUT) {
		dev_info(dev, "CPU %d: software timeout during scan\n", cpu);
	} else if (status.error_code == IFS_SW_PARTIAL_COMPLETION) {
		dev_info(dev, "CPU %d: %s\n", cpu,
			 "Not all scan chunks were executed. Maximum forward progress retries exceeded");
	} else {
		dev_info(dev, "CPU %d: SCAN unknown status %llx\n", cpu, status.data);
	}
}

static void message_fail(struct device *dev, int cpu, union ifs_status status)
{
	struct ifs_data *ifsd = ifs_get_data(dev);

	/*
	 * signature_error is set when the output from the scan chains does not
	 * match the expected signature. This might be a transient problem (e.g.
	 * due to a bit flip from an alpha particle or neutron). If the problem
	 * repeats on a subsequent test, then it indicates an actual problem in
	 * the core being tested.
	 */
	if (status.signature_error) {
		dev_err(dev, "CPU %d: test signature incorrect. Batch: %02x version: 0x%x\n",
			cpu, ifsd->cur_batch, ifsd->loaded_version);
	}
}

static bool can_restart(union ifs_status status)
{
	enum ifs_status_err_code err_code = status.error_code;

	/* Signature for chunk is bad, or scan test failed */
	if (status.signature_error || status.control_error)
		return false;

	switch (err_code) {
	case IFS_NO_ERROR:
	case IFS_OTHER_THREAD_COULD_NOT_JOIN:
	case IFS_INTERRUPTED_BEFORE_RENDEZVOUS:
	case IFS_POWER_MGMT_INADEQUATE_FOR_SCAN:
	case IFS_EXCEED_NUMBER_OF_THREADS_CONCURRENT:
	case IFS_INTERRUPTED_DURING_EXECUTION:
		return true;
	case IFS_INVALID_CHUNK_RANGE:
	case IFS_MISMATCH_ARGUMENTS_BETWEEN_THREADS:
	case IFS_CORE_NOT_CAPABLE_CURRENTLY:
	case IFS_UNASSIGNED_ERROR_CODE:
	case IFS_UNASSIGNED_ERROR_CODE_0xA:
	case IFS_CORRUPTED_CHUNK:
		break;
	}
	return false;
}

#define SPINUNIT 100 /* 100 nsec */
static atomic_t array_cpus_in;
static atomic_t scan_cpus_in;

/*
 * Simplified cpu sibling rendezvous loop based on microcode loader __wait_for_cpus()
 */
static void wait_for_sibling_cpu(struct ifs_data *ifsd, atomic_t *t, long long timeout)
{
	int all_cpus = cpumask_weight(&ifsd->grp_cpumask);

	atomic_inc(t);
	while (atomic_read(t) < all_cpus) {
		if (timeout < SPINUNIT)
			return;
		ndelay(SPINUNIT);
		timeout -= SPINUNIT;
		touch_nmi_watchdog();
	}
}

/*
 * Execute the scan. Called "simultaneously" on all threads of a core
 * at high priority using the stop_cpus mechanism.
 */
static int doscan(void *data)
{
	int cpu = smp_processor_id(), start, stop;
	struct ifs_test_output *pcpu_scan;
	struct run_params *params = data;
	union ifs_status status;
	struct ifs_data *ifsd;

	ifsd = params->ifsd;
	pcpu_scan = this_cpu_ptr(ifsd->result_ptr);

	if (ifsd->generation) {
		start = params->activate->gen2.start;
		stop = params->activate->gen2.stop;
	} else {
		start = params->activate->gen0.start;
		stop = params->activate->gen0.stop;
	}

	wait_for_sibling_cpu(ifsd, &scan_cpus_in, NSEC_PER_SEC);

	/*
	 * This WRMSR will wait for other HT threads to also write
	 * to this MSR (at most for activate.delay cycles). Then it
	 * starts scan of each requested chunk. The core scan happens
	 * during the "execution" of the WRMSR. This instruction can
	 * take up to 200 milliseconds (in the case where all chunks
	 * are processed in a single pass) before it retires.
	 */
	wrmsrl(MSR_ACTIVATE_SCAN, params->activate->data);
	rdmsrl(MSR_SCAN_STATUS, status.data);

	trace_ifs_status(ifsd->cur_batch, start, stop, status.data);

	/* Pass back the result of the scan */
	if (cpu == ifsd->cpu)
		params->status = status;

	pcpu_scan->test_details = status.data;

	return 0;
}

#define get_scan_status(stat) ((union ifs_status *)(&((stat)->test_details)))

static void update_group_status(struct device *dev, int reason)
{
	struct ifs_data *ifsd = ifs_get_data(dev);
	struct ifs_test_output *pcpu_scan;
	union ifs_status *scan_status;
	int lcpu;

	for_each_cpu(lcpu, &ifsd->grp_cpumask) {
		pcpu_scan = per_cpu_ptr(ifsd->result_ptr, lcpu);
		scan_status = get_scan_status(pcpu_scan);
		scan_status->error_code = reason;
	}
}

static bool can_restart_test_group(struct device *dev)
{
	struct ifs_data *ifsd = ifs_get_data(dev);
	struct ifs_test_output *pcpu_scan;
	union ifs_status *scan_status;
	int lcpu;

	for_each_cpu(lcpu, &ifsd->grp_cpumask) {
		pcpu_scan = per_cpu_ptr(ifsd->result_ptr, lcpu);
		scan_status = get_scan_status(pcpu_scan);
		if (!can_restart(*scan_status))
			return false;
	}
	return true;
}

static void update_group_result(struct device *dev, int stop_chunk)
{
	struct ifs_data *ifsd = ifs_get_data(dev);
	struct ifs_test_output *pcpu_scan;
	union ifs_status *scan_status;
	u32 reached_chunk;
	int lcpu;

	for_each_cpu(lcpu, &ifsd->grp_cpumask) {
		pcpu_scan = per_cpu_ptr(ifsd->result_ptr, lcpu);
		scan_status = get_scan_status(pcpu_scan);
		reached_chunk = ifsd->generation ? scan_status->gen2.chunk_num :
				scan_status->gen0.chunk_num;

		if (scan_status->signature_error) {
			pcpu_scan->test_result = SCAN_TEST_FAIL;
			message_fail(dev, lcpu, *scan_status);
		} else if (scan_status->control_error || scan_status->error_code ||
			   reached_chunk <= stop_chunk) {
			pcpu_scan->test_result = SCAN_NOT_TESTED;
			message_not_tested(dev, lcpu, *scan_status);
		} else{
			pcpu_scan->test_result = SCAN_TEST_PASS;
		}

		if (lcpu == ifsd->cpu)
			ifsd->status = pcpu_scan->test_result;
	}
}

/*
 * Use stop_core_cpuslocked() to synchronize writing to MSR_ACTIVATE_SCAN
 * on all threads of the core to be tested. Loop if necessary to complete
 * run of all chunks. Include some defensive tests to make sure forward
 * progress is made, and that the whole test completes in a reasonable time.
 */
static void ifs_test_core(int cpu, struct device *dev)
{
	union ifs_status status = {};
	union ifs_scan activate;
	unsigned long timeout;
	struct ifs_data *ifsd;
	int to_start, to_stop;
	int status_chunk;
	struct run_params params;
	int retries;

	ifsd = ifs_get_data(dev);
	ifsd->cpu = cpu;

	activate.gen0.rsvd = 0;
	activate.delay = IFS_THREAD_WAIT;
	activate.sigmce = 0;
	to_start = 0;
	to_stop = ifsd->valid_chunks - 1;

	params.ifsd = ifs_get_data(dev);

	if (ifsd->generation) {
		activate.gen2.start = to_start;
		activate.gen2.stop = to_stop;
	} else {
		activate.gen0.start = to_start;
		activate.gen0.stop = to_stop;
	}

	timeout = jiffies + HZ / 2;
	retries = MAX_IFS_RETRIES;

	while (to_start <= to_stop) {
		if (time_after(jiffies, timeout)) {
			status.error_code = IFS_SW_TIMEOUT;
			update_group_status(dev, IFS_SW_TIMEOUT);
			break;
		}

		params.activate = &activate;
		atomic_set(&scan_cpus_in, 0);

		if (ifsd->all_lp_join)
			stop_cluster_cpuslocked(cpu, doscan, &params);
		else
			stop_core_cpuslocked(cpu, doscan, &params);

		status = params.status;

		/* Some cases can be retried, give up for others */
		if (!can_restart_test_group(dev))
			break;

		status_chunk = ifsd->generation ? status.gen2.chunk_num : status.gen0.chunk_num;
		if (status_chunk == to_start) {
			/* Check for forward progress */
			if (--retries == 0) {
				if (status.error_code == IFS_NO_ERROR) {
					status.error_code = IFS_SW_PARTIAL_COMPLETION;
					update_group_status(dev, IFS_SW_PARTIAL_COMPLETION);
				}
				break;
			}
		} else {
			retries = MAX_IFS_RETRIES;
			if (ifsd->generation)
				activate.gen2.start = status_chunk;
			else
				activate.gen0.start = status_chunk;
			to_start = status_chunk;
		}
	}

	/* Update status for this core */
	ifsd->scan_details = status.data;
	update_group_result(dev, to_stop);
}

static int do_array_test(void *data)
{
	struct run_array_params *params = data;
	int cpu = smp_processor_id();
	union ifs_array *command;
	struct ifs_data *ifsd;

	ifsd = params->ifsd;

	command = params->command;
	wait_for_sibling_cpu(ifsd, &array_cpus_in, NSEC_PER_SEC);

	/*
	 * Only one logical CPU on a core needs to trigger the Array test via MSR write.
	 */
	if (cpu == ifsd->cpu) {
		wrmsrl(MSR_ARRAY_BIST, command->data);
		/* Pass back the result of the test */
		rdmsrl(MSR_ARRAY_BIST, command->data);
	}

	return 0;
}

static void ifs_array_test_core(int cpu, struct device *dev)
{
	struct run_array_params params;
	union ifs_array command = {};
	bool timed_out = false;
	struct ifs_data *ifsd;
	unsigned long timeout;

	ifsd = ifs_get_data(dev);
	ifsd->cpu = cpu;
	params.ifsd = ifsd;

	command.array_bitmask = ~0U;
	timeout = jiffies + HZ / 2;

	do {
		if (time_after(jiffies, timeout)) {
			timed_out = true;
			break;
		}
		atomic_set(&array_cpus_in, 0);
		params.command = &command;

		if (ifsd->all_lp_join)
			stop_cluster_cpuslocked(cpu, do_array_test, &params);
		else
			stop_core_cpuslocked(cpu, do_array_test, &params);

		if (command.ctrl_result)
			break;
	} while (command.array_bitmask);

	ifsd->scan_details = command.data;

	if (command.ctrl_result)
		ifsd->status = SCAN_TEST_FAIL;
	else if (timed_out || command.array_bitmask)
		ifsd->status = SCAN_NOT_TESTED;
	else
		ifsd->status = SCAN_TEST_PASS;
}

#define ARRAY_GEN1_TEST_ALL_ARRAYS	0x0ULL
#define ARRAY_GEN1_STATUS_FAIL		0x1ULL

static int do_array_test_gen1(void *status)
{
	int cpu = smp_processor_id();
	int first;

	first = cpumask_first(cpu_smt_mask(cpu));

	if (cpu == first) {
		wrmsrl(MSR_ARRAY_TRIGGER, ARRAY_GEN1_TEST_ALL_ARRAYS);
		rdmsrl(MSR_ARRAY_STATUS, *((u64 *)status));
	}

	return 0;
}

static void ifs_array_test_gen1(int cpu, struct device *dev)
{
	struct ifs_data *ifsd = ifs_get_data(dev);
	u64 status = 0;

	stop_core_cpuslocked(cpu, do_array_test_gen1, &status);
	ifsd->scan_details = status;

	if (status & ARRAY_GEN1_STATUS_FAIL)
		ifsd->status = SCAN_TEST_FAIL;
	else
		ifsd->status = SCAN_TEST_PASS;
}

static void build_cpugroup_mask(struct device *dev, int cpu)
{
	struct ifs_data *ifsd = ifs_get_data(dev);

	cpumask_clear(&ifsd->grp_cpumask);

	/*
	 * In some platforms, more than one core shares the same SCAN
	 * engine, but doesn't require a rendezvous.
	 */
	if (!ifsd->all_lp_join) {
		cpumask_set_cpu(cpu, &ifsd->grp_cpumask);
		return;
	}
	cpumask_copy(&ifsd->grp_cpumask, topology_cluster_cpumask(cpu));
}

/*
 * Initiate per core test. It wakes up work queue threads on the target cpu and
 * its sibling cpu. Once all sibling threads wake up, the scan test gets executed and
 * wait for all sibling threads to finish the scan test.
 */
int do_core_test(int cpu, struct device *dev)
{
	const struct ifs_test_caps *test = ifs_get_test_caps(dev);
	struct ifs_data *ifsd = ifs_get_data(dev);
	int ret = 0;

	/* Prevent CPUs from being taken offline during the scan test */
	cpus_read_lock();
	build_cpugroup_mask(dev, cpu);

	if (!cpu_online(cpu)) {
		dev_info(dev, "cannot test on the offline cpu %d\n", cpu);
		ret = -EINVAL;
		goto out;
	}

	switch (test->test_num) {
	case IFS_TYPE_SAF:
		if (!ifsd->loaded)
			ret = -EPERM;
		else
			ifs_test_core(cpu, dev);
		break;
	case IFS_TYPE_ARRAY_BIST:
		if (ifsd->array_gen == ARRAY_GEN0)
			ifs_array_test_core(cpu, dev);
		else
			ifs_array_test_gen1(cpu, dev);
		break;
	default:
		ret = -EINVAL;
	}
out:
	cpus_read_unlock();
	return ret;
}
