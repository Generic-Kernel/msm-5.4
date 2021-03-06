// SPDX-License-Identifier: GPL-2.0-only
/*
 * Hibernation support specific for ARM
 *
 * Derived from work on ARM hibernation support by:
 *
 * Ubuntu project, hibernation support for mach-dove
 * Copyright (C) 2010 Nokia Corporation (Hiroshi Doyu)
 * Copyright (C) 2010 Texas Instruments, Inc. (Teerth Reddy et al.)
 *  https://lkml.org/lkml/2010/6/18/4
 *  https://lists.linux-foundation.org/pipermail/linux-pm/2010-June/027422.html
 *  https://patchwork.kernel.org/patch/96442/
 *
 * Copyright (C) 2006 Rafael J. Wysocki <rjw@sisk.pl>
 */

#include <linux/mm.h>
#include <linux/suspend.h>
#include <asm/system_misc.h>
#include <asm/idmap.h>
#include <asm/suspend.h>
#include <asm/memory.h>
#include <asm/sections.h>
#include "reboot.h"

#ifdef CONFIG_ARCH_HIBERNATION_HEADER
#include <linux/utsname.h>
#include <linux/version.h>
#include <asm/smp.h>
#include <asm/smp_plat.h>

/*
 * The logical cpu number we should resume on, initialised to a non-cpu
 * number.
 */
static int sleep_cpu = -EINVAL;

/*
 * Values that may not change over hibernate/resume. We put the build number
 * and date in here so that we guarantee not to resume with a different
 * kernel.
 */
struct arch_hibernate_hdr_invariants {
	char		uts_version[__NEW_UTS_LEN + 1];
};

/* These values need to be know across a hibernate/restore. */
static struct arch_hibernate_hdr {
	struct arch_hibernate_hdr_invariants invariants;

	/* These are needed to find the relocated kernel if built with kaslr */
	u64		ttbr1_el1;
	void		(*reenter_kernel)(void);
	u64		phys_reenter_kernel;
	/*
	 * We need to know where the __hyp_stub_vectors are after restore to
	 * re-configure el2.
	 */
	u64	__hyp_stub_vectors;
	u64		sleep_cpu_mpidr;
} resume_hdr;

static inline void arch_hdr_invariants(struct arch_hibernate_hdr_invariants *i)
{
	memset(i, 0, sizeof(*i));
	memcpy(i->uts_version, init_utsname()->version, sizeof(i->uts_version));
}

int arch_hibernation_header_save(void *addr, unsigned int max_size)
{
	struct arch_hibernate_hdr *hdr = addr;

	if (max_size < sizeof(*hdr))
		return -EOVERFLOW;

	arch_hdr_invariants(&hdr->invariants);
	hdr->ttbr1_el1		= __pa_symbol(swapper_pg_dir);
	hdr->reenter_kernel	= cpu_resume;
	hdr->phys_reenter_kernel  = __pa(cpu_resume);

	/* Save the mpidr of the cpu we called cpu_suspend() on... */
	if (sleep_cpu < 0) {
		pr_err("Failing to hibernate on an unknown CPU.\n");
		return -ENODEV;
	}
	hdr->sleep_cpu_mpidr = cpu_logical_map(sleep_cpu);
	pr_info("Hibernating on CPU %d [mpidr:0x%llx]\n", sleep_cpu,
		hdr->sleep_cpu_mpidr);

	return 0;
}
EXPORT_SYMBOL(arch_hibernation_header_save);

int arch_hibernation_header_restore(void *addr)
{
	int ret;
	struct arch_hibernate_hdr_invariants invariants;
	struct arch_hibernate_hdr *hdr = addr;

	arch_hdr_invariants(&invariants);
	if (memcmp(&hdr->invariants, &invariants, sizeof(invariants))) {
		pr_crit("Hibernate image not generated by this kernel!\n");
		return -EINVAL;
	}

	sleep_cpu = get_logical_index(hdr->sleep_cpu_mpidr);
	pr_info("Hibernated on CPU %d [mpidr:0x%llx]\n", sleep_cpu,
		hdr->sleep_cpu_mpidr);
	if (sleep_cpu < 0) {
		pr_crit("Hibernated on a CPU not known to this kernel!\n");
		sleep_cpu = -EINVAL;
		return -EINVAL;
	}
	if (!cpu_online(sleep_cpu)) {
		pr_info("Hibernated on a CPU that is offline! Bringing CPU up.\n");
		ret = cpu_up(sleep_cpu);
		if (ret) {
			pr_err("Failed to bring hibernate-CPU up!\n");
			sleep_cpu = -EINVAL;
			return ret;
		}
	}

	resume_hdr = *hdr;

	return 0;
}
EXPORT_SYMBOL(arch_hibernation_header_restore);
#endif

int pfn_is_nosave(unsigned long pfn)
{
	unsigned long nosave_begin_pfn = virt_to_pfn(&__nosave_begin);
	unsigned long nosave_end_pfn = virt_to_pfn(&__nosave_end - 1);

	return (pfn >= nosave_begin_pfn) && (pfn <= nosave_end_pfn);
}

void notrace save_processor_state(void)
{
	WARN_ON(num_online_cpus() != 1);
	local_fiq_disable();
}

void notrace restore_processor_state(void)
{
	local_fiq_enable();
}

/*
 * Snapshot kernel memory and reset the system.
 *
 * swsusp_save() is executed in the suspend finisher so that the CPU
 * context pointer and memory are part of the saved image, which is
 * required by the resume kernel image to restart execution from
 * swsusp_arch_suspend().
 *
 * soft_restart is not technically needed, but is used to get success
 * returned from cpu_suspend.
 *
 * When soft reboot completes, the hibernation snapshot is written out.
 */
static int notrace arch_save_image(unsigned long unused)
{
	int ret;

#ifdef CONFIG_ARCH_HIBERNATION_HEADER
	sleep_cpu = smp_processor_id();
#endif
	ret = swsusp_save();
	if (ret == 0)
		_soft_restart(virt_to_idmap(cpu_resume), false);
	return ret;
}

/*
 * Save the current CPU state before suspend / poweroff.
 */
int notrace swsusp_arch_suspend(void)
{
	return cpu_suspend(0, arch_save_image);
}

/*
 * Restore page contents for physical pages that were in use during loading
 * hibernation image.  Switch to idmap_pgd so the physical page tables
 * are overwritten with the same contents.
 */
static void notrace arch_restore_image(void *unused)
{
	struct pbe *pbe;

	cpu_switch_mm(idmap_pgd, &init_mm);
	for (pbe = restore_pblist; pbe; pbe = pbe->next)
		copy_page(pbe->orig_address, pbe->address);

	_soft_restart(virt_to_idmap(cpu_resume), false);
}

static u64 resume_stack[PAGE_SIZE/2/sizeof(u64)] __nosavedata;

/*
 * Resume from the hibernation image.
 * Due to the kernel heap / data restore, stack contents change underneath
 * and that would make function calls impossible; switch to a temporary
 * stack within the nosave region to avoid that problem.
 */
int swsusp_arch_resume(void)
{
	call_with_stack(arch_restore_image, 0,
		resume_stack + ARRAY_SIZE(resume_stack));
	return 0;
}
