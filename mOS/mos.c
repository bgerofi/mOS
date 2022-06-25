/*
 * Multi Operating System (mOS)
 * Copyright (c) 2016-2017 Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/init.h>
#include <linux/printk.h>
#include <linux/syscalls.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/mos.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <asm/current.h>
#include <asm/setup.h>

#include "lwkcpu.h"
#include "lwkctrl.h"
#include "mosras.h"

#ifdef CONFIG_MOS_FOR_HPC

#undef pr_fmt
#define pr_fmt(fmt)	"mOS: " fmt

#define MOS_VERSION	"0.7"

static cpumask_var_t lwkcpus_map;
static cpumask_var_t utility_cpus_map;
static cpumask_var_t lwkcpus_reserved_map;
static char *lwkauto;
static DEFINE_MUTEX(mos_sysfs_mutex);
static LIST_HEAD(mos_process_option_callbacks);
static LIST_HEAD(mos_process_callbacks);

/* NOTE: The following items are not static.  They are referenced
 *       by other LWK components in mOS.
 */

DEFINE_PER_CPU(cpumask_t, mos_syscall_mask);
DEFINE_PER_CPU(cpumask_t, lwkcpus_mask);

struct mos_process_callbacks_elem_t {
	struct list_head list;
	struct mos_process_callbacks_t *callbacks;
};

int mos_register_process_callbacks(struct mos_process_callbacks_t *cbs)
{
	struct mos_process_callbacks_elem_t *elem;

	if (!cbs)
		return -EINVAL;

	elem = vmalloc(sizeof(struct mos_process_callbacks_elem_t));

	if (!elem)
		return -ENOMEM;

	elem->callbacks = cbs;
	list_add(&elem->list, &mos_process_callbacks);

	return 0;
}

int mos_unregister_process_callbacks(struct mos_process_callbacks_t *cbs)
{
	struct mos_process_callbacks_elem_t *elem;

	if (!cbs)
		return -EINVAL;

	list_for_each_entry(elem, &mos_process_callbacks, list) {
		if (elem->callbacks == cbs) {
			list_del(&elem->list);
			vfree(elem);
			return 0;
		}
	}

	return -EINVAL;
}

struct mos_process_option_callback_elem_t {
	struct list_head list;
	char name[64];
	int (*callback)(const char *, struct mos_process_t *);
};

int mos_register_option_callback(const char *name,
	 int (*cb)(const char *, struct mos_process_t *))
{
	struct mos_process_option_callback_elem_t *elem;

	if (!cb)
		return -EINVAL;

	if (strlen(name) >= sizeof(elem->name))
		return -EINVAL;

	elem = vmalloc(sizeof(struct mos_process_option_callback_elem_t));

	if (!elem)
		return -ENOMEM;

	strcpy(elem->name, name);
	elem->callback = cb;
	list_add(&elem->list, &mos_process_option_callbacks);

	return 0;
}

int mos_unregister_option_callback(const char *name,
		   int (*cb)(const char *, struct mos_process_t *))
{
	struct mos_process_option_callback_elem_t *elem;

	if (!cb)
		return -EINVAL;

	list_for_each_entry(elem, &mos_process_option_callbacks, list) {
		if (elem->callback == cb && strcmp(name, elem->name) == 0) {
			list_del(&elem->list);
			vfree(elem);
			return 0;
		}
	}

	return -EINVAL;
}

void get_mos_view_cpumask(struct cpumask *dst, const struct cpumask *src)
{
	if (IS_MOS_VIEW(current, MOS_VIEW_LWK_LOCAL))
		cpumask_and(dst, src, current->mos_process->lwkcpus);
	else {
		if (IS_MOS_VIEW(current, MOS_VIEW_LINUX))
			cpumask_andnot(dst, src, cpu_lwkcpus_mask);
		else if (IS_MOS_VIEW(current, MOS_VIEW_LWK))
			cpumask_and(dst, src, cpu_lwkcpus_mask);
		else
			cpumask_copy(dst, src);
	}
}

ssize_t cpumap_print_mos_view_cpumask(bool list, char *buf,
				const struct cpumask *mask)
{
	ssize_t ret;
	cpumask_var_t mos_view_cpumask;

	if (!alloc_cpumask_var(&mos_view_cpumask, GFP_KERNEL))
		return -ENOMEM;

	get_mos_view_cpumask(mos_view_cpumask, mask);

	ret = cpumap_print_to_pagebuf(list, buf, mos_view_cpumask);
	free_cpumask_var(mos_view_cpumask);
	return ret;
}

#ifdef MOS_DEBUG_PROCESS
static void _mos_debug_process(struct mos_process_t *p, const char *func,
			       const int line)
{
	if (!p) {
		pr_info("[%s:%d] NULL process", func, line);
		return;
	}
	scnprintf(cpulist_buf, sizeof(cpulist_buf), "%*pbl", cpumask_pr_args(p->lwkcpus));
	pr_info("[%s:%d] tgid=%d lwkcpu=%s alive=%d\n", func,
		line, p->tgid, cpulist_buf, atomic_read(&p->alive));
	scnprintf(cpulist_buf, sizeof(cpulist_buf), "%*pbl", cpumask_pr_args(p->utilcpus));
	pr_info("[%s:%d] tgid=%d utilcpu=%s p@=%p\n", func, line, p->tgid,
		cpulist_buf, p);
}
#else
#define _mos_debug_process(a, b, c)
#endif

/**
 * Find the MOS process associated with the current thread.
 * Create the entry if one does not already exist.
 */
static struct mos_process_t *mos_get_process(void)
{
	struct mos_process_t *process = current->mos_process;

	if (!process) {
		struct mos_process_callbacks_elem_t *elem;

		process = vmalloc(sizeof(struct mos_process_t));
		if (!process)
			return 0;
		process->tgid = current->tgid;

		if (!zalloc_cpumask_var(&process->lwkcpus, GFP_KERNEL) ||
		    !zalloc_cpumask_var(&process->utilcpus, GFP_KERNEL)) {
			mos_ras(MOS_LWK_PROCESS_ERROR_UNSTABLE_NODE,
				"CPU mask allocation failure.");
			return 0;
		}

		process->lwkcpus_sequence = 0;
		process->num_lwkcpus = 0;
		process->num_util_threads = 0;

		atomic_set(&process->alive, 1); /* count the current thread */

		list_for_each_entry(elem, &mos_process_callbacks, list) {
			if (elem->callbacks->mos_process_init &&
			    elem->callbacks->mos_process_init(process)) {
				mos_ras(MOS_LWK_PROCESS_ERROR,
					"Non-zero return code from LWK process initialization callback %pf.",
					elem->callbacks->mos_process_init);
				process = 0;
				break;
			}
		}

	}

	return process;
}

void mos_exit_thread(void)
{
	struct mos_process_t *process;
	struct mos_process_callbacks_elem_t *elem;

	mutex_lock(&mos_sysfs_mutex);

	process = current->mos_process;

	if (!process) {
		mos_ras(MOS_LWK_PROCESS_ERROR,
			"Unexpected NULL LWK process object pointer encountered in %s().",
			__func__);
		goto unlock;
	}

	_mos_debug_process(process, __func__, __LINE__);

	list_for_each_entry(elem, &mos_process_callbacks, list) {
		if (elem->callbacks->mos_thread_exit)
			elem->callbacks->mos_thread_exit(process);
	}

	/* Wait for the last thread to shut down before cleaning up. */
	if (!atomic_dec_and_test(&process->alive))
		goto unlock;

	_mos_debug_process(process, __func__, __LINE__);

	list_for_each_entry(elem, &mos_process_callbacks, list) {
		if (elem->callbacks->mos_process_exit)
			elem->callbacks->mos_process_exit(process);
	}

	/* Release the resources reserved by this process. */

	cpumask_xor(lwkcpus_reserved_map, lwkcpus_reserved_map,
		    process->lwkcpus);

	/* Free process resources. */
	free_cpumask_var(process->lwkcpus);
	free_cpumask_var(process->utilcpus);
	vfree(process->lwkcpus_sequence);
	vfree(process);

unlock:
	mutex_unlock(&mos_sysfs_mutex);
}

/**
 * An operations structure for modifying various mOS sysfs
 * files.  This allows us to compose various types of operations
 * and file types.
 */
struct mos_sysfs_mask_write_op {
	int (*parser)(const char *, cpumask_var_t);
	int (*operation)(cpumask_var_t);
} mos_sysfs_mask_write_op;

/**
 * A parameterized write operations for mOS sysfs files.  The buf/count
 * arguments are parsed via the op->parser field.  Then the op->operation
 * is applied under the safety of the mos_sysfs_mutex.
 */

static ssize_t mos_sysfs_mask_write(const char *buf, size_t count,
				    struct mos_sysfs_mask_write_op *op)
{
	cpumask_var_t reqmask;
	int rc;

	if (!zalloc_cpumask_var(&reqmask, GFP_KERNEL))
		return -ENOMEM;

	if (op->parser(buf, reqmask)) {
		pr_info("Could not parse %s\n", buf);
		count = -EINVAL;
		goto out;
	}

	mutex_lock(&mos_sysfs_mutex);

	rc = op->operation(reqmask);

	if (rc < 0)
		count = rc;

	mutex_unlock(&mos_sysfs_mutex);

out:
	free_cpumask_var(reqmask);
	return count;

}

/**
 * _xxx_cpus_reserved = request
 * Return -EINVAL if request is not a subset of the lwkcpus.  Otherwise
 * copy the request into the target and return 0.
 */

static int _cpus_reserved_set(cpumask_var_t request, cpumask_var_t target)
{
	int rc = 0;

	if (!cpumask_empty(request) && !cpumask_subset(request, lwkcpus_map)) {
		pr_info("Non-LWK CPU was requested.\n");
		rc = -EINVAL;
		goto out;
	}

	cpumask_copy(target, request);

out:
	return rc;
}

static int _lwkcpus_reserved_set(cpumask_var_t request)
{
	return _cpus_reserved_set(request, lwkcpus_reserved_map);
}

/**
 * xxx_reserved |= request
 * Return -EINVAL if request is not a subset of the designated
 * LWK CPUs (lwkcpus_maps).  Return -EBUSY if the requested set
 * overlaps with the reserved compute CPUs.
 * Otherwise, update the target with the requested set.
 */

static int _cpus_request_set(cpumask_var_t request, cpumask_var_t target)
{
	int rc = 0;

	if (!cpumask_subset(request, lwkcpus_map)) {
		pr_info("Non-LWK CPU was requested.\n");
		rc = -EINVAL;
		goto out;
	}

	if (cpumask_intersects(request, lwkcpus_reserved_map)) {
		rc = -EBUSY;
		goto out;
	}

	cpumask_or(target, target, request);

	current->mos_flags |= MOS_IS_LWK_PROCESS;
out:
	return rc;
}

static int _lwkcpus_request_set(cpumask_var_t request)
{
	int rc;

	rc = _cpus_request_set(request, lwkcpus_reserved_map);

	if (!rc) {
		int *cpu_list, num_lwkcpus, cpu;

		current->mos_process = mos_get_process();

		if (!current->mos_process) {
			rc = -ENOMEM;
			goto out;
		}
		cpumask_or(current->mos_process->lwkcpus, request, request);

		/* Allocate the CPU sequence array */
		num_lwkcpus = cpumask_weight(current->mos_process->lwkcpus);
		cpu_list = vmalloc(sizeof(int)*(num_lwkcpus+1));
		if (!cpu_list) {
			rc = -ENOMEM;
			goto out;
		}
		current->mos_process->num_lwkcpus = num_lwkcpus;
		current->mos_process->lwkcpus_sequence = cpu_list;

		/* We use the mm pointer as a marker. It will change when yod
		** execv() into the application process. We can use this marker
		** to tell whether yod or the LWK process is calling
		** lwk_sys_brk() for example.
		*/
		current->mos_process->yod_mm = current->mm;

		/* Initialize the sequencing array based on the lwkcpus mask */
		for_each_cpu(cpu, current->mos_process->lwkcpus)
			*cpu_list++ = cpu;

		/* Set sentinel value */
		*cpu_list = -1;

		/* Create a mask within the process of all utility CPUs */
		cpumask_or(current->mos_process->utilcpus,
			   current->mos_process->utilcpus,
			   utility_cpus_map);

		_mos_debug_process(current->mos_process, __func__, __LINE__);
	}

 out:
	return rc;
}

static struct kobject *mos_kobj;

static ssize_t show_cpu_list(cpumask_var_t cpus, char *buff)
{
	ssize_t n;

	n = scnprintf(buff, PAGE_SIZE, "%*pbl", cpumask_pr_args(cpus));
	if (n >= 0) {
		buff[n++] = '\n';
		buff[n] = 0;
	}
	return n;
}

static ssize_t show_cpu_mask(cpumask_var_t cpus, char *buff)
{
	ssize_t n;

	n = scnprintf(buff, PAGE_SIZE, "%*pb", cpumask_pr_args(cpus));
	if (n >= 0) {
		buff[n++] = '\n';
		buff[n] = 0;
	}
	return n;
}

static ssize_t version_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buff)
{
	return scnprintf(buff, PAGE_SIZE, "%s\n", MOS_VERSION);
}

#define MOS_SYSFS_CPU_SHOW_LIST(name)				\
	static ssize_t name##_show(struct kobject *kobj,	\
				   struct kobj_attribute *attr,	\
				   char *buff)			\
	{							\
		return show_cpu_list(name##_map, buff);		\
	}

#define MOS_SYSFS_CPU_SHOW_MASK(name)					\
	static ssize_t name##_mask_show(struct kobject *kobj,		\
					struct kobj_attribute *attr,	\
					char *buff)			\
	{								\
		return show_cpu_mask(name##_map, buff);		\
	}

#define MOS_SYSFS_CPU_STORE_LIST(name)					\
	static struct mos_sysfs_mask_write_op name##_op = {		\
		.parser = cpulist_parse,				\
		.operation = _##name##_set,				\
	};								\
									\
	static ssize_t name##_store(struct kobject *kobj,		\
				    struct kobj_attribute *attr,	\
				    const char *buf, size_t count)	\
	{								\
		return mos_sysfs_mask_write(buf, count, &name##_op);	\
	}								\

#define MOS_SYSFS_CPU_STORE_MASK(name) \
	static struct mos_sysfs_mask_write_op name##_mask_op = {	\
		.parser = cpumask_parse,				\
		.operation = _##name##_set,				\
	};								\
									\
	static ssize_t name##_mask_store(struct kobject *kobj,		\
					 struct kobj_attribute *attr,	\
					 const char *buf, size_t count)	\
	{								\
		return mos_sysfs_mask_write(buf, count, &name##_mask_op); \
	}								\


#define MOS_SYSFS_CPU_RO(name)				\
	MOS_SYSFS_CPU_SHOW_LIST(name)			\
	MOS_SYSFS_CPU_SHOW_MASK(name)			\
	static struct kobj_attribute name##_attr =	\
		__ATTR_RO(name);			\
	static struct kobj_attribute name##_mask_attr = \
		__ATTR_RO(name##_mask)			\

#define MOS_SYSFS_CPU_RW(name)				\
	MOS_SYSFS_CPU_SHOW_LIST(name)			\
	MOS_SYSFS_CPU_SHOW_MASK(name)			\
	MOS_SYSFS_CPU_STORE_LIST(name)			\
	MOS_SYSFS_CPU_STORE_MASK(name)			\
	static struct kobj_attribute name##_attr =	\
		__ATTR_RW(name);			\
	static struct kobj_attribute name##_mask_attr =	\
		__ATTR_RW(name##_mask)			\

#define MOS_SYSFS_CPU_WO(name)				\
	MOS_SYSFS_CPU_STORE_LIST(name)			\
	MOS_SYSFS_CPU_STORE_MASK(name)			\
	static struct kobj_attribute name##_attr =	\
		__ATTR_WO(name);			\
	static struct kobj_attribute name##_mask_attr =	\
		__ATTR_WO(name##_mask)			\

MOS_SYSFS_CPU_RO(lwkcpus);
MOS_SYSFS_CPU_RW(lwkcpus_reserved);
MOS_SYSFS_CPU_WO(lwkcpus_request);
MOS_SYSFS_CPU_RO(utility_cpus);

#define MAX_NIDS (1 << CONFIG_NODES_SHIFT)

static ssize_t _lwkmem_vec_show(char *buff, int (*getter)(unsigned long *, size_t *), unsigned long deflt)
{
	unsigned long lwkm[MAX_NIDS];
	size_t  i, n;
	ssize_t len;
	int rc;

	if (getter) {
		n = ARRAY_SIZE(lwkm);
		rc = getter(lwkm, &n);
		if (rc)
			return -EINVAL;
	} else {
		lwkm[0] = deflt ? deflt : 0;
		n = 1;
	}

	len = 0;
	buff[0] = 0;

	for (i = 0; i < n; i++)
		len += scnprintf(buff + len, PAGE_SIZE - len, "%lu ", lwkm[i]);

	buff[len] = '\n';
	return len;
}

static int _lwkmem_vec_parse(char *buff, unsigned long *lwkm, size_t *n, unsigned long *total)
{
	char *val, *bptr;
	size_t capacity = *n;
	int rc;

	bptr = buff;
	*total = 0;
	*n = 0;

	while ((val = strsep(&bptr, " "))) {

		if (*n == capacity) {
			mos_ras(MOS_LWK_PROCESS_ERROR,
				"Potential overflow in lwkmem_request buffer (capacity=%ld).",
				capacity);
			return -EINVAL;
		}

		rc = kstrtoul(val, 0, lwkm + *n);

		if (rc) {
			mos_ras(MOS_LWK_PROCESS_ERROR,
				"Attempted to write invalid value (%s) to lwkmem_request.",
				val);
			return -EINVAL;
		}

		*total += lwkm[*n];
		(*n)++;
	}

	return *n > 0 ? 0 : -EINVAL;
}

static ssize_t lwkmem_show(struct kobject *kobj,
			   struct kobj_attribute *attr, char *buff)
{
	return _lwkmem_vec_show(buff, lwkmem_get, 0);
}

static ssize_t lwkmem_reserved_show(struct kobject *kobj,
			   struct kobj_attribute *attr, char *buff)
{
	return _lwkmem_vec_show(buff, lwkmem_reserved_get, 0);
}

static ssize_t lwkmem_request_store(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    const char *buff, size_t count)
{
	int rc;
	unsigned long lwkm[MAX_NIDS], total;
	size_t n;
	char *str;
	struct mos_process_t *process;

	str = kstrdup(buff, GFP_KERNEL);

	if (!str) {
		rc = -ENOMEM;
		goto out;
	}

	n = ARRAY_SIZE(lwkm);

	rc = _lwkmem_vec_parse(str, lwkm, &n, &total);

	if (rc)
		goto out;

	mutex_lock(&mos_sysfs_mutex);

	current->mos_flags |= MOS_IS_LWK_PROCESS;

	rc = count;

	process = mos_get_process();

	if (!process) {
		rc = -ENOMEM;
		goto unlock;
	}

	if (lwkmem_request) {
		if (lwkmem_request(process, lwkm, n)) {
			rc = -EBUSY;
			goto unlock;
		}
	}

	_mos_debug_process(process, __func__, __LINE__);

 unlock:
	mutex_unlock(&mos_sysfs_mutex);

 out:
	kfree(str);
	return rc;
}

static ssize_t lwk_util_threads_store(struct kobject *kobj,
				  struct kobj_attribute *attr,
				  const char *buff, size_t count)
{
	struct mos_process_t *proc = current->mos_process;
	int num_util_threads;

	if (!proc) {
		mos_ras(MOS_LWK_PROCESS_ERROR,
			"Attempted to set the number of utility threads from non-LWK process.");
		return  -EINVAL;
	}
	if (kstrtoint(buff, 0, &num_util_threads) || (num_util_threads < 0)) {
		mos_ras(MOS_LWK_PROCESS_ERROR,
			"Attempted to write an invalid value (%s) to the LWK utility thread count.",
			buff);
		return -EINVAL;
	}

	proc->num_util_threads = num_util_threads;

	return count;
}

static ssize_t lwkprocesses_show(struct kobject *kobj,
			   struct kobj_attribute *attr, char *buff)
{
	char *current_buff = buff;
	int remaining_buffsize = PAGE_SIZE;
	int bytes_written = 0;
	int total_bytes_written = 0;
	struct task_struct *task;

	mutex_lock(&mos_sysfs_mutex);
	read_lock(&tasklist_lock);

	for_each_process(task) {
		if (task->mos_process) {
			bytes_written = scnprintf(current_buff,
						  remaining_buffsize, "%u,",
						  task->tgid);
			remaining_buffsize -= bytes_written;
			current_buff += bytes_written;
			total_bytes_written += bytes_written;
		}
	}

	read_unlock(&tasklist_lock);
	mutex_unlock(&mos_sysfs_mutex);

	/* Replace trailing comma with newline character. the
	   scnprintf already stored the required NULL string termination */
	if (bytes_written > 0)
		*(--current_buff) = '\n';
	else /* If no processes in the list, terminate the empty string */
		*buff = '\0';

	return total_bytes_written;
}

static ssize_t lwkcpus_sequence_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buff, size_t count)
{
	unsigned cpuid;
	int *cpu_ptr;
	int cpus_in_list = 0;
	char *str, *str_orig = 0;
	char *val;
	size_t rc = count;
	struct mos_process_t *proc = current->mos_process;

	if (!proc) {
		mos_ras(MOS_LWK_PROCESS_ERROR,
			"Attempted to write an LWK CPU sequence for a non-LWK process.");
		rc = -EINVAL;
		goto out;
	}
	cpu_ptr = proc->lwkcpus_sequence;
	if (!cpu_ptr) {
		mos_ras(MOS_LWK_PROCESS_ERROR,
			"Attempted to write an LWK CPU sequence prior to reserving LWK CPUs.");
		rc = -EINVAL;
		goto out;
	}
	str = kstrndup(buff, count, GFP_KERNEL);
	if (!str) {
		rc = -ENOMEM;
		goto out;
	}
	str_orig = str;
	while ((val = strsep(&str, ","))) {
		int kresult = kstrtouint(val, 0, &cpuid);

		if (kresult) {
			mos_ras(MOS_LWK_PROCESS_ERROR,
				"Attempted to write invalid value to the LWK CPU sequence (rc=%d).",
				kresult);
			rc = -EINVAL;
			goto out;
		}
		/* Store CPU id into the integer array */
		if (++cpus_in_list > proc->num_lwkcpus) {
			rc = -EINVAL;
			mos_ras(MOS_LWK_PROCESS_ERROR,
				"Too many CPUs were provided in an LWK sequence list.");
			goto out;
		}
		*cpu_ptr++ = cpuid;
	}
	if (cpus_in_list < proc->num_lwkcpus) {
		mos_ras(MOS_LWK_PROCESS_ERROR,
			"Too few CPUs were provided in an LWK sequence list.");
		rc = -EINVAL;
	}
out:
	kfree(str_orig);
	return rc;
}

static ssize_t lwk_options_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buff, size_t count)
{
	ssize_t rc = count;
	char *option = 0, *value;
	const char *name = buff;
	struct mos_process_t *mosp = current->mos_process;
	struct mos_process_option_callback_elem_t *elem;
	struct mos_process_callbacks_elem_t *cbs;
	bool not_found;

	if (!mosp) {
		mos_ras(MOS_LWK_PROCESS_ERROR,
			"Attempted to set LWK options for a non-LWK process.");
		rc = -EINVAL;
		goto out;
	}

	/* Options are stored in the buffer as a sequence of strings,
	 * separated by a null character.  This possibly includes a
	 * leading null character.  The end of the sequence is identified
	 * by two null characters.
	 */

	if (*name == '\0')
		name++;

	while (strlen(name)) {

		pr_debug("(*) %s: option=\"%s\"\n", __func__, name);

		option = kstrndup(name, count, GFP_KERNEL);

		if (!option) {
			rc = -ENOMEM;
			goto out;
		}

		value = strchr(option, '=');
		if (value)
			*value++ = '\0';

		not_found = true;
		list_for_each_entry(elem, &mos_process_option_callbacks, list) {
			if (strcmp(elem->name, option) == 0) {
				rc = elem->callback(value, mosp);
				if (rc) {
					mos_ras(MOS_LWK_PROCESS_ERROR,
						"Option callback %s / %pf reported an error (rc=%ld).",
						elem->name, elem->callback, rc);
					rc = -EINVAL;
					goto out;
				}
				not_found = false;
				break;
			}
		}

		if (not_found) {
			mos_ras(MOS_LWK_PROCESS_ERROR,
				"No option callback found for %s\n", option);
			rc = -EINVAL;
			goto out;
		}

		name += strlen(name) + 1;

		if (name - buff > count) {
			mos_ras(MOS_LWK_PROCESS_ERROR,
				"Overflow in options buffer.");
			rc = -EINVAL;
			goto out;
		}

		kfree(option);
		option = NULL;
	}

	list_for_each_entry(cbs, &mos_process_callbacks, list) {
		if (cbs->callbacks->mos_process_start &&
		    cbs->callbacks->mos_process_start(mosp)) {
			mos_ras(MOS_LWK_PROCESS_ERROR,
				"Non-zero return code from process start callback %pf\n",
				cbs->callbacks->mos_process_start);
			rc = -EINVAL;
			goto out;
		}
	}

	rc = count;
 out:
	kfree(option);
	return rc;
}

static ssize_t lwkmem_domain_info_store(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    const char *buff, size_t count)
{
	ssize_t rc;
	unsigned long nids[MAX_NIDS];
	size_t n;
	char *str, *typ_str, *nids_str, *nid_str;
	enum lwkmem_type_t typ;
	struct mos_process_t *mosp = current->mos_process;

	pr_debug("(>) %s buff=\"%s\" count=%ld\n", __func__, buff, count);

	mutex_lock(&mos_sysfs_mutex);

	if (!mosp) {
		mos_ras(MOS_LWK_PROCESS_ERROR,
			"Attempted to set domain information for a non-LWK process.");
		rc = -EINVAL;
		goto out;
	}

	str = kstrdup(buff, GFP_KERNEL);

	if (!str) {
		rc = -ENOMEM;
		goto out;
	}

	/* Information is passed along as a space-delimited sequence of
	 * <type>=<nid>[,<nid>...] phrases.  Each phrase is parsed, converted
	 * to an array of NIDs and lwkmem_type_t, and ultimately passed along
	 * to the memory subsystem.
	 */
	while ((typ_str = strsep(&str, " "))) {

		if (strlen(typ_str) == 0)
			continue;

		nids_str = strchr(typ_str, '=');

		if (!nids_str) {
			rc = -EINVAL;
			goto out;
		}

		*nids_str++ = '\0';

		if (strcmp(typ_str, "hbm") == 0)
			typ = lwkmem_hbm;
		else if (strcmp(typ_str, "dram") == 0)
			typ = lwkmem_dram;
		else if (strcmp(typ_str, "nvram") == 0)
			typ = lwkmem_nvram;
		else {
			rc = -EINVAL;
			mos_ras(MOS_LWK_PROCESS_ERROR,
				"Unrecognized memory type: %s.", typ_str);
			goto out;
		}

		n = 0;

		while ((nid_str = strsep(&nids_str, ","))) {

			if (n == MAX_NIDS) {
				mos_ras(MOS_LWK_PROCESS_ERROR,
					"Overflow in lwkmem_domain_info buffer.");
				rc = -EINVAL;
				goto out;
			}

			rc = kstrtoul(nid_str, 0, nids + n);

			if (rc) {
				mos_ras(MOS_LWK_PROCESS_ERROR,
					"Attempted to write invalid value to lwkmem_domain_info: %s.",
					nid_str);
				rc = -EINVAL;
				goto out;
			}

			n++;

		}

		if (lwkmem_set_domain_info) {
			rc = lwkmem_set_domain_info(mosp, typ, nids, n);
			if (rc) {
				mos_ras(MOS_LWK_PROCESS_ERROR,
					"Non-zero return code %ld from lwkmem_set_domain_info.",
					rc);
				rc = -EINVAL;
				goto out;
			}
		}
	}

	rc = count;
 out:
	mutex_unlock(&mos_sysfs_mutex);
	kfree(str);
	return rc;
}

static int validate_lwkcpus_spec(char *lwkcpus_parm)
{
	cpumask_var_t to, from, new_lwkcpus, new_syscallcpus;
	char *mutable_param_start, *mutable_param, *s_to, *s_from;
	int rc = -1;

	mutable_param_start = mutable_param = kstrdup(lwkcpus_parm, GFP_KERNEL);
	if (!mutable_param) {
		mos_ras(MOS_LWKCTL_FAILURE,
			"Failure duplicating CPU param_value string in %s.",
			__func__);
		return rc;
	}
	if (!zalloc_cpumask_var(&to, GFP_KERNEL) ||
	    !zalloc_cpumask_var(&from, GFP_KERNEL) ||
	    !zalloc_cpumask_var(&new_syscallcpus, GFP_KERNEL) ||
	    !zalloc_cpumask_var(&new_lwkcpus, GFP_KERNEL)) {
		mos_ras(MOS_LWKCTL_FAILURE, "Could not allocate cpumasks.");
		kfree(mutable_param_start);
		return -1;
	}
	while ((s_to = strsep(&mutable_param, ":"))) {
		if (!(s_from = strchr(s_to, '.'))) {
			/* No syscall target defined */
			s_from = s_to;
			s_to = strchr(s_to, '\0');
		} else
			*(s_from++) = '\0';
		if (cpulist_parse(s_to, to) < 0 ||
		cpulist_parse(s_from, from) < 0) {
			mos_ras(MOS_LWKCTL_FAILURE,
				"Invalid character in CPU specification.");
			goto out;
		}
		/* Maximum of one syscall target CPU allowed per LWKCPU range */
		if ((cpumask_weight(to) > 1) && !cpumask_empty(from)) {
			mos_ras(MOS_LWKCTL_FAILURE,
				"More than one syscall target CPU specified.");
			goto out;
		}
		/* Build the set of lwk CPUs */
		cpumask_or(new_lwkcpus, new_lwkcpus, from);
		/* Build the set of syscall CPUs */
		cpumask_or(new_syscallcpus, new_syscallcpus, to);
	}
	if (cpumask_intersects(new_lwkcpus, cpu_online_mask)) {
		mos_ras(MOS_LWKCTL_FAILURE,
			"Overlap detected. LWK CPUs: %*pbl Online CPUs: %*pbl.",
			cpumask_pr_args(new_lwkcpus),
			cpumask_pr_args(cpu_online_mask));
		goto out;
	}
	if (cpumask_intersects(new_lwkcpus, new_syscallcpus)) {
		mos_ras(MOS_LWKCTL_FAILURE,
			"Overlap detected. LWK CPUs: %*pbl syscall CPUs: %*pbl.",
			cpumask_pr_args(new_lwkcpus),
			cpumask_pr_args(new_syscallcpus));
		goto out;
	}
	rc = 0;
out:
	free_cpumask_var(to);
	free_cpumask_var(from);
	free_cpumask_var(new_lwkcpus);
	free_cpumask_var(new_syscallcpus);
	kfree(mutable_param_start);
	return rc;
}

/*
 * The specifed LWK CPUs should be in the Linux off-line state when called
 *
 * example input string:
 *	1.2-7,9:10.11,13,14
 *		In the above example, CPU 1 will be the syscall target
 *		for LWK CPUS 2,3,4,5,6,7,9 and CPU 10 will be the target
 *		for LWK CPUS 10,11,13,14
 */
int lwk_config_lwkcpus(char *param_value, char *lwkcpu_profile)
{
	cpumask_var_t to;
	cpumask_var_t from;
	cpumask_var_t new_utilcpus;
	cpumask_var_t new_lwkcpus;
	cpumask_var_t back_to_linux;

	unsigned cpu;
	char *s_to, *s_from;
	int rc = -1;
	bool return_cpus;
	char *mutable_param_start, *mutable_param;

	mutable_param_start = mutable_param = kstrdup(param_value, GFP_KERNEL);

	if (!mutable_param) {
		mos_ras(MOS_LWKCTL_FAILURE,
			"Failure duplicating CPU param_value string in %s.",
			__func__);
		return rc;
	}

	return_cpus = (param_value[0] == '\0') ? 1 : 0;

	if (!cpumask_empty(lwkcpus_map) && !return_cpus) {
		mos_ras(MOS_LWKCTL_FAILURE,
			"Attempt to modify existing LWKCPU configuration. Not supported.");
		kfree(mutable_param_start);
		return rc;
	}

	if (!zalloc_cpumask_var(&to, GFP_KERNEL) ||
	    !zalloc_cpumask_var(&from, GFP_KERNEL) ||
	    !zalloc_cpumask_var(&back_to_linux, GFP_KERNEL) ||
	    !zalloc_cpumask_var(&new_utilcpus, GFP_KERNEL) ||
		!zalloc_cpumask_var(&new_lwkcpus, GFP_KERNEL)) {
		mos_ras(MOS_LWKCTL_FAILURE, "Could not allocate cpumasks.");
		kfree(mutable_param_start);
		return rc;
	}

	if (return_cpus) {
		/* Save the mask of LWK CPUs being returned */
		cpumask_copy(back_to_linux, lwkcpus_map);
		/* Remove syscall migrations from the currently configured LWK CPUs */
		for_each_cpu(cpu, lwkcpus_map) {
			cpumask_t *syscall_mask;

			syscall_mask = per_cpu_ptr(&mos_syscall_mask, cpu);
			cpumask_clear(to);
			cpumask_set_cpu(cpu, to);
			cpumask_copy(syscall_mask, to);
		}
		pr_info("Returning CPUs to Linux: %*pbl\n",
				cpumask_pr_args(back_to_linux));

	} else {

		if (validate_lwkcpus_spec(param_value))
			goto out;

		while ((s_to = strsep(&mutable_param, ":"))) {
			if (!(s_from = strchr(s_to, '.'))) {
				/* No syscall target defined */
				s_from = s_to;
				s_to = strchr(s_to, '\0');
			} else
				*(s_from++) = '\0';
			cpulist_parse(s_to, to);
			cpulist_parse(s_from, from);
			for_each_cpu(cpu, from) {
				cpumask_t *mask;

				mask = per_cpu_ptr(&mos_syscall_mask, cpu);
				if (!cpumask_weight(to)) {
					cpumask_clear(mask);
					cpumask_set_cpu(cpu, mask);
				} else
					cpumask_copy(mask, to);
			}
			if (cpumask_weight(to) == 0) {
				pr_info(
				    "LWK CPUs %*pbl will not ship syscalls to Linux\n",
				    cpumask_pr_args(from));
			} else if (!cpumask_empty(from)) {
				pr_info(
				    "LWK CPUs %*pbl will ship syscalls to Linux CPU %*pbl\n",
				    cpumask_pr_args(from),
				    cpumask_pr_args(to));
			}
			/* Build the set of LWK and Utility CPUs */
			cpumask_or(new_lwkcpus, new_lwkcpus, from);
			cpumask_or(new_utilcpus, new_utilcpus, to);
		}
		pr_info("Configured LWK CPUs: %*pbl\n",
				cpumask_pr_args(new_lwkcpus));
		pr_info("Configured Utility CPUs: %*pbl\n",
		  cpumask_pr_args(new_utilcpus));

	}

	if (!cpumask_empty(back_to_linux)) {
		rc = lwkcpu_partition_destroy(back_to_linux);
		if (!rc)
			lwkcpu_state_deinit();
	}
	/* Let each CPU have its own copy of the lwkcpus mask. This gets
	 * interrogated on each system call.
	 */
	for_each_possible_cpu(cpu)
		cpumask_copy(per_cpu_ptr(&lwkcpus_mask, cpu), new_lwkcpus);

	if (!cpumask_empty(new_lwkcpus)) {
		bool profile_set = false;

		if (lwkcpu_profile) {
			if (lwkcpu_state_init(lwkcpu_profile)) {
				mos_ras(MOS_LWKCTL_WARNING,
					"Failed to set lwkcpu_profile: %s.",
					lwkcpu_profile);
			} else
				profile_set = true;
		}

		if (!profile_set)
			if (lwkcpu_state_init(LWKCPU_PROF_NOR))
				mos_ras(MOS_LWKCTL_WARNING,
					"Failed to set lwkcpu_profile: %s.",
					LWKCPU_PROF_NOR);

		rc = lwkcpu_partition_create(new_lwkcpus);
	}

	if (rc)
		goto out;

	/* Update the sysfs cpu masks */
	cpumask_clear(lwkcpus_map);
	cpumask_clear(utility_cpus_map);
	cpumask_copy(lwkcpus_map, new_lwkcpus);
	cpumask_copy(utility_cpus_map, new_utilcpus);

	rc = 0;
out:
	free_cpumask_var(to);
	free_cpumask_var(from);
	free_cpumask_var(new_utilcpus);
	free_cpumask_var(new_lwkcpus);
	free_cpumask_var(back_to_linux);
	kfree(mutable_param_start);
	return rc;
}

int lwk_config_lwkmem(char *param_value)
{
	int rc = -EINVAL;

	if (lwkmem_static_enabled)
		goto out;

	if (param_value[0] == '\0')
		rc = lwkmem_partition_destroy();
	else
		rc = lwkmem_partition_create(param_value);
out:
	return rc;
}

static int lwk_validate_auto(char *auto_s)
{
	int rc = 0;
	char *tmp_start, *tmp, *resource;

	tmp_start = tmp = kstrdup(auto_s, GFP_KERNEL);
	if (!tmp_start)
		return -1;
	while ((resource = strsep(&tmp, ","))) {
		if (strcmp(resource, "cpu") && strcmp(resource, "mem")) {
			rc = -1;
			break;
		}
	}
	kfree(tmp_start);
	return rc;
}

static ssize_t lwk_config_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buff, size_t count)
{
	ssize_t rc = -EINVAL;
	char *tmp, *tmp_start, *s_keyword, *s_value;
	char *lwkcpus, *lwkcpu_profile, *lwkmem, *auto_config;
	bool delete_lwkcpu, delete_lwkmem;

	lwkcpus = lwkcpu_profile = lwkmem = auto_config = NULL;
	delete_lwkcpu = delete_lwkmem = false;
	mutex_lock(&mos_sysfs_mutex);

	tmp_start = tmp = kstrdup(buff, GFP_KERNEL);
	if (!tmp)
		goto out;

	while ((s_keyword = strsep(&tmp, " "))) {
		if (strlen(s_keyword) == 0)
			continue;
		if (!(s_value = strchr(s_keyword, '='))) {
			mos_ras(MOS_LWKCTL_FAILURE,
				"Failed to find sign[=] to set a keyword: %s.",
				s_keyword);
			goto out;
		}
		*s_value++ = '\0';
		if (*s_value == '\n')
			*s_value = '\0';
		if (strcmp(s_keyword, "lwkcpus") == 0) {
			kfree(lwkcpus);
			strreplace(s_value, '\n', '\0');
			lwkcpus = kstrdup(s_value, GFP_KERNEL);
			if (!lwkcpus)
				goto out;
			delete_lwkcpu = lwkcpus[0] == '\0';
		} else if (!strcmp(s_keyword, "lwkcpu_profile")) {
			kfree(lwkcpu_profile);
			strreplace(s_value, '\n', '\0');
			lwkcpu_profile = kstrdup(s_value, GFP_KERNEL);
			if (!lwkcpu_profile)
				goto out;
		} else if (!strcmp(s_keyword, "lwkmem")) {
			kfree(lwkmem);
			strreplace(s_value, '\n', '\0');
			lwkmem = kstrdup(s_value, GFP_KERNEL);
			if (!lwkmem)
				goto out;
			delete_lwkmem = lwkmem[0] == '\0';
		} else if (!strcmp(s_keyword, "auto")) {
			kfree(auto_config);
			strreplace(s_value, '\n', '\0');
			auto_config = kstrdup(s_value, GFP_KERNEL);
			if (!auto_config)
				goto out;
		} else {
			mos_ras(MOS_LWKCTL_WARNING,
				"Unsupported keyword: %s was ignored.",
				s_keyword);
		}
	}
	if (auto_config && lwk_validate_auto(auto_config)) {
		mos_ras(MOS_LWKCTL_FAILURE,
			"Unsupported auto configuration data=%s", auto_config);
		kfree(auto_config);
		goto out;
	}
	kfree(lwkauto);
	lwkauto = auto_config;

	if (lwkcpus && lwkmem && !lwkmem_static_enabled) {
		if (delete_lwkcpu != delete_lwkmem) {
			mos_ras(MOS_LWKCTL_FAILURE,
				"Can not create %s and delete %s partition.",
				delete_lwkcpu ? "lwkmem" : "lwkcpu",
				delete_lwkcpu ? "lwkcpu" : "lwkmem");
			goto out;
		}

		if (delete_lwkcpu) {
			if (lwk_config_lwkcpus(lwkcpus, lwkcpu_profile) < 0) {
				mos_ras(MOS_LWKCTL_FAILURE,
					"Failure processing: lwkcpus=%s",
					lwkcpus);
				goto out;
			}
			if (lwk_config_lwkmem(lwkmem) < 0) {
				mos_ras(MOS_LWKCTL_FAILURE,
					"Failure processing: lwkmem=%s",
					lwkmem);
				goto out;
			}
		} else {
			if (lwk_config_lwkmem(lwkmem) < 0) {
				mos_ras(MOS_LWKCTL_FAILURE,
					"Failure processing: lwkmem=%s",
					lwkmem);
				goto out;
			}
			if (lwk_config_lwkcpus(lwkcpus, lwkcpu_profile) < 0) {
				mos_ras(MOS_LWKCTL_FAILURE,
					"Failure processing: lwkcpus=%s",
					lwkcpus);
				goto out;
			}
		}
		/* Update LWKCPU and LWKCPU profile specs */
		snprintf(lwkctrl_cpus_spec, LWKCTRL_CPUS_SPECSZ,
			 "%s", lwkcpus);

		if (delete_lwkcpu)
			lwkctrl_cpu_profile_spec[0] = '\0';
		else {
			if (lwkcpu_profile &&
			    strcmp(lwkcpu_profile, LWKCPU_PROF_NOR) &&
			    strcmp(lwkcpu_profile, LWKCPU_PROF_DBG)) {
				snprintf(lwkctrl_cpu_profile_spec,
					LWKCTRL_CPU_PROFILE_SPECSZ, "%s",
					LWKCPU_PROF_NOR);
			} else {
				snprintf(lwkctrl_cpu_profile_spec,
					LWKCTRL_CPU_PROFILE_SPECSZ, "%s",
					lwkcpu_profile ?
					lwkcpu_profile : LWKCPU_PROF_NOR);
			}
		}
		rc = count;
	} else {
		if (!lwkmem_static_enabled) {
			mos_ras(MOS_LWKCTL_FAILURE,
				"Can not execute %s specification alone.",
				lwkcpus ? "lwkcpus" : "lwkmem");
			goto out;
		}

		if (lwkcpus) {
			if (lwk_config_lwkcpus(lwkcpus, lwkcpu_profile) < 0) {
				mos_ras(MOS_LWKCTL_FAILURE,
					"Failure processing: lwkcpus=%s",
					lwkcpus);
				goto out;
			}
			/* Update LWKCPU and LWKCPU profile specs */
			snprintf(lwkctrl_cpus_spec, LWKCTRL_CPUS_SPECSZ,
				 "%s", lwkcpus);

			if (delete_lwkcpu)
				lwkctrl_cpu_profile_spec[0] = '\0';
			else {
				if (lwkcpu_profile &&
				    strcmp(lwkcpu_profile, LWKCPU_PROF_NOR) &&
				    strcmp(lwkcpu_profile, LWKCPU_PROF_DBG)) {
					snprintf(lwkctrl_cpu_profile_spec,
						LWKCTRL_CPU_PROFILE_SPECSZ,
						"%s", LWKCPU_PROF_NOR);
				} else {
					snprintf(lwkctrl_cpu_profile_spec,
						LWKCTRL_CPU_PROFILE_SPECSZ,
						"%s", lwkcpu_profile ?
						lwkcpu_profile :
						LWKCPU_PROF_NOR);
				}
			}
			rc = count;
		}

		if (lwkmem) {
			mos_ras(MOS_LWKCTL_FAILURE,
				"Cannot create lwkmem=%s.  Partition is static.",
				lwkmem);
			rc = lwkcpus ? rc : -EINVAL;
		}
	}
out:
	kfree(lwkcpus);
	kfree(lwkcpu_profile);
	kfree(lwkmem);
	kfree(tmp_start);
	mutex_unlock(&mos_sysfs_mutex);
	return rc;
}

static ssize_t lwk_config_show(struct kobject *kobj,
			       struct kobj_attribute *attr,
			       char *buff)
{
	int buffsize = PAGE_SIZE;
	int rc = -ENOMEM;
	size_t auto_ssize;
	char *cur_buff = buff;
	char *auto_s;

	mutex_lock(&mos_sysfs_mutex);

	if (!lwkauto)
		auto_s = kstrdup("", GFP_KERNEL);
	else {
		auto_ssize = strlen(lwkauto) + sizeof(" auto=");
		auto_s = kmalloc(auto_ssize, GFP_KERNEL);
		if (!auto_s)
			goto out;
		snprintf(auto_s, auto_ssize, " auto=%s", lwkauto);
	}
	rc = scnprintf(cur_buff, buffsize,
			"lwkcpus=%s lwkcpu_profile=%s lwkmem=%s%s\n",
			lwkctrl_cpus_spec, lwkctrl_cpu_profile_spec,
			lwkmem_get_spec(), auto_s);
out:
	kfree(auto_s);
	mutex_unlock(&mos_sysfs_mutex);
	return rc;
}

static struct kobj_attribute version_attr = __ATTR_RO(version);
static struct kobj_attribute lwkmem_attr = __ATTR_RO(lwkmem);
static struct kobj_attribute lwkmem_reserved_attr = __ATTR_RO(lwkmem_reserved);
static struct kobj_attribute lwkmem_request_attr = __ATTR_WO(lwkmem_request);
static struct kobj_attribute lwkprocesses_attr = __ATTR_RO(lwkprocesses);
static struct kobj_attribute lwkcpus_sequence_attr =
						__ATTR_WO(lwkcpus_sequence);
static struct kobj_attribute lwk_util_threads_attr =
						__ATTR_WO(lwk_util_threads);
static struct kobj_attribute lwk_options_attr = __ATTR_WO(lwk_options);
static struct kobj_attribute lwkmem_domain_info_attr =
						__ATTR_WO(lwkmem_domain_info);
static struct kobj_attribute lwk_config_attr = __ATTR_RW(lwk_config);
static  struct attribute *mos_attributes[] = {
	&version_attr.attr,
	&lwkcpus_attr.attr,
	&lwkcpus_mask_attr.attr,
	&lwkcpus_reserved_attr.attr,
	&lwkcpus_reserved_mask_attr.attr,
	&lwkcpus_request_attr.attr,
	&lwkcpus_request_mask_attr.attr,
	&lwkmem_attr.attr,
	&lwkmem_reserved_attr.attr,
	&lwkmem_request_attr.attr,
	&lwkprocesses_attr.attr,
	&lwkcpus_sequence_attr.attr,
	&lwk_util_threads_attr.attr,
	&lwk_options_attr.attr,
	&lwkmem_domain_info_attr.attr,
	&utility_cpus_attr.attr,
	&utility_cpus_mask_attr.attr,
	&lwk_config_attr.attr,
	NULL
};

static struct attribute_group mos_attr_group = {
	.attrs = mos_attributes,
};

static int __init mos_sysfs_init(void)
{

	int ret;

	if (!zalloc_cpumask_var(&lwkcpus_map, GFP_KERNEL) ||
	    !zalloc_cpumask_var(&utility_cpus_map, GFP_KERNEL) ||
	    !zalloc_cpumask_var(&lwkcpus_reserved_map, GFP_KERNEL)) {
		mos_ras(MOS_BOOT_ERROR,
			"%s: CPU mask allocation failed.", __func__);
		ret = -ENOMEM;
		goto out;
	}

	mos_kobj = kobject_create_and_add("mOS", kernel_kobj);

	if (!mos_kobj) {
		ret = -ENOMEM;
		goto out;
	}

	lwkcpus_request_attr.attr.mode |= S_IWGRP;
	lwkcpus_request_mask_attr.attr.mode |= S_IWGRP;
	lwkmem_domain_info_attr.attr.mode |= S_IWGRP;
	lwkmem_request_attr.attr.mode |= S_IWGRP;
	lwkcpus_sequence_attr.attr.mode |= S_IWGRP;
	lwk_options_attr.attr.mode |= S_IWGRP;
	lwk_util_threads_attr.attr.mode |= S_IWGRP;

	ret = sysfs_create_group(mos_kobj, &mos_attr_group);
	if (ret) {
		mos_ras(MOS_BOOT_ERROR,
			"%s: Could not create sysfs entries for mOS.",
			__func__);
		goto out;
	}

	ret = mosras_sysfs_init(mos_kobj);

	if (ret)
		goto out;

	return 0;

out:
	return ret;
}

subsys_initcall(mos_sysfs_init);

#endif /* CONFIG_MOS_FOR_HPC */
