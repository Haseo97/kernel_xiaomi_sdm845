/*
 * Contains CPU specific errata definitions
 *
 * Copyright (C) 2014 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/types.h>
#include <asm/alternative.h>
#include <asm/cachetype.h>
#include <linux/cpu.h>
#include <asm/cpu.h>
#include <asm/cputype.h>
#include <asm/cpufeature.h>

static bool __maybe_unused
is_affected_midr_range(const struct arm64_cpu_capabilities *entry,
			    int scope)
{
	WARN_ON(scope != SCOPE_LOCAL_CPU || preemptible());
	return is_midr_in_range(read_cpuid_id(), &entry->midr_range);
}

static bool
has_mismatched_cache_type(const struct arm64_cpu_capabilities *entry,
			  int scope)
{
	u64 mask = CTR_CACHE_MINLINE_MASK;

	/* Skip matching the min line sizes for cache type check */
	if (entry->capability == ARM64_MISMATCHED_CACHE_TYPE)
		mask ^= arm64_ftr_reg_ctrel0.strict_mask;

	WARN_ON(scope != SCOPE_LOCAL_CPU || preemptible());
	return (read_cpuid_cachetype() & mask) !=
	       (arm64_ftr_reg_ctrel0.sys_val & mask);
}

static void
cpu_enable_trap_ctr_access(const struct arm64_cpu_capabilities *__unused)
{
	/* Clear SCTLR_EL1.UCT */
	config_sctlr_el1(SCTLR_EL1_UCT, 0);
}

#include <asm/mmu_context.h>
#include <asm/cacheflush.h>

DEFINE_PER_CPU_READ_MOSTLY(struct bp_hardening_data, bp_hardening_data);

#ifdef CONFIG_KVM
extern char __smccc_workaround_1_smc_start[];
extern char __smccc_workaround_1_smc_end[];
extern char __smccc_workaround_1_hvc_start[];
extern char __smccc_workaround_1_hvc_end[];

static void __copy_hyp_vect_bpi(int slot, const char *hyp_vecs_start,
				const char *hyp_vecs_end)
{
	void *dst = __bp_harden_hyp_vecs_start + slot * SZ_2K;
	int i;

	for (i = 0; i < SZ_2K; i += 0x80)
		memcpy(dst + i, hyp_vecs_start, hyp_vecs_end - hyp_vecs_start);

	flush_icache_range((uintptr_t)dst, (uintptr_t)dst + SZ_2K);
}

static void install_bp_hardening_cb(bp_hardening_cb_t fn,
				    const char *hyp_vecs_start,
				    const char *hyp_vecs_end)
{
	static int last_slot = -1;
	static DEFINE_SPINLOCK(bp_lock);
	int cpu, slot = -1;

	spin_lock(&bp_lock);
	for_each_possible_cpu(cpu) {
		if (per_cpu(bp_hardening_data.fn, cpu) == fn) {
			slot = per_cpu(bp_hardening_data.hyp_vectors_slot, cpu);
			break;
		}
	}

	if (slot == -1) {
		last_slot++;
		BUG_ON(((__bp_harden_hyp_vecs_end - __bp_harden_hyp_vecs_start)
			/ SZ_2K) <= last_slot);
		slot = last_slot;
		__copy_hyp_vect_bpi(slot, hyp_vecs_start, hyp_vecs_end);
	}

	__this_cpu_write(bp_hardening_data.hyp_vectors_slot, slot);
	__this_cpu_write(bp_hardening_data.fn, fn);
	spin_unlock(&bp_lock);
}
#else
#define __smccc_workaround_1_smc_start		NULL
#define __smccc_workaround_1_smc_end		NULL
#define __smccc_workaround_1_hvc_start		NULL
#define __smccc_workaround_1_hvc_end		NULL

static void install_bp_hardening_cb(bp_hardening_cb_t fn,
				      const char *hyp_vecs_start,
				      const char *hyp_vecs_end)
{
	__this_cpu_write(bp_hardening_data.fn, fn);
}
#endif	/* CONFIG_KVM */

#include <uapi/linux/psci.h>
#include <linux/arm-smccc.h>
#include <linux/psci.h>

static void call_smc_arch_workaround_1(void)
{
	arm_smccc_1_1_smc(ARM_SMCCC_ARCH_WORKAROUND_1, NULL);
}

static void call_hvc_arch_workaround_1(void)
{
	arm_smccc_1_1_hvc(ARM_SMCCC_ARCH_WORKAROUND_1, NULL);
}

static bool __nospectre_v2;
static int __init parse_nospectre_v2(char *str)
{
	__nospectre_v2 = true;
	return 0;
}
early_param("nospectre_v2", parse_nospectre_v2);

/*
 * -1: No workaround
 *  0: No workaround required
 *  1: Workaround installed
 */
static int detect_harden_bp_fw(void)
{
	bp_hardening_cb_t cb;
	void *smccc_start, *smccc_end;
	struct arm_smccc_res res;

	if (psci_ops.smccc_version == SMCCC_VERSION_1_0)
		return -1;

	switch (psci_ops.conduit) {
	case PSCI_CONDUIT_HVC:
		arm_smccc_1_1_hvc(ARM_SMCCC_ARCH_FEATURES_FUNC_ID,
				  ARM_SMCCC_ARCH_WORKAROUND_1, &res);
		switch ((int)res.a0) {
		case 1:
			/* Firmware says we're just fine */
			return 0;
		case 0:
			cb = call_hvc_arch_workaround_1;
			/* This is a guest, no need to patch KVM vectors */
			smccc_start = NULL;
			smccc_end = NULL;
			break;
		default:
			return -1;
		}
		break;

	case PSCI_CONDUIT_SMC:
		arm_smccc_1_1_smc(ARM_SMCCC_ARCH_FEATURES_FUNC_ID,
				  ARM_SMCCC_ARCH_WORKAROUND_1, &res);
		switch ((int)res.a0) {
		case 1:
			/* Firmware says we're just fine */
			return 0;
		case 0:
			cb = call_smc_arch_workaround_1;
			smccc_start = __smccc_workaround_1_smc_start;
			smccc_end = __smccc_workaround_1_smc_end;
			break;
		default:
			return -1;
		}
		break;

	default:
		return -1;
	}

	if (IS_ENABLED(CONFIG_HARDEN_BRANCH_PREDICTOR))
		install_bp_hardening_cb(cb, smccc_start, smccc_end);

	return 1;
}

DEFINE_PER_CPU_READ_MOSTLY(u64, arm64_ssbd_callback_required);

int ssbd_state __read_mostly = ARM64_SSBD_KERNEL;
static bool __ssb_safe = true;

static const struct ssbd_options {
	const char	*str;
	int		state;
} ssbd_options[] = {
	{ "force-on",	ARM64_SSBD_FORCE_ENABLE, },
	{ "force-off",	ARM64_SSBD_FORCE_DISABLE, },
	{ "kernel",	ARM64_SSBD_KERNEL, },
};

static int __init ssbd_cfg(char *buf)
{
	int i;

	if (!buf || !buf[0])
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(ssbd_options); i++) {
		int len = strlen(ssbd_options[i].str);

		if (strncmp(buf, ssbd_options[i].str, len))
			continue;

		ssbd_state = ssbd_options[i].state;
		return 0;
	}

	return -EINVAL;
}
early_param("ssbd", ssbd_cfg);

void __init arm64_update_smccc_conduit(struct alt_instr *alt,
				       __le32 *origptr, __le32 *updptr,
				       int nr_inst)
{
	u32 insn;

	BUG_ON(nr_inst != 1);

	switch (psci_ops.conduit) {
	case PSCI_CONDUIT_HVC:
		insn = aarch64_insn_get_hvc_value();
		break;
	case PSCI_CONDUIT_SMC:
		insn = aarch64_insn_get_smc_value();
		break;
	default:
		return;
	}

	*updptr = cpu_to_le32(insn);
}

__attribute__((used))
alternative_cb_t arm64_update_smccc_conduit_indirectly_callable(void)
{
	return &arm64_update_smccc_conduit;
}

void __init arm64_enable_wa2_handling(struct alt_instr *alt,
				      __le32 *origptr, __le32 *updptr,
				      int nr_inst)
{
	BUG_ON(nr_inst != 1);
	/*
	 * Only allow mitigation on EL1 entry/exit and guest
	 * ARCH_WORKAROUND_2 handling if the SSBD state allows it to
	 * be flipped.
	 */
	if (arm64_get_ssbd_state() == ARM64_SSBD_KERNEL)
		*updptr = cpu_to_le32(aarch64_insn_gen_nop());
}

__attribute__((used))
alternative_cb_t arm64_enable_wa2_handling_indirectly_callable(void)
{
	return &arm64_enable_wa2_handling;
}

void arm64_set_ssbd_mitigation(bool state)
{
	if (!IS_ENABLED(CONFIG_ARM64_SSBD)) {
		pr_info_once("SSBD disabled by kernel configuration\n");
		return;
	}

	switch (psci_ops.conduit) {
	case PSCI_CONDUIT_HVC:
		arm_smccc_1_1_hvc(ARM_SMCCC_ARCH_WORKAROUND_2, state, NULL);
		break;

	case PSCI_CONDUIT_SMC:
		arm_smccc_1_1_smc(ARM_SMCCC_ARCH_WORKAROUND_2, state, NULL);
		break;

	default:
		WARN_ON_ONCE(1);
		break;
	}
}

static bool has_ssbd_mitigation(const struct arm64_cpu_capabilities *entry,
				    int scope)
{
	struct arm_smccc_res res;
	bool required = true;
	s32 val;
	bool this_cpu_safe = false;

	WARN_ON(scope != SCOPE_LOCAL_CPU || preemptible());

	if (cpu_mitigations_off())
		ssbd_state = ARM64_SSBD_FORCE_DISABLE;

	/* delay setting __ssb_safe until we get a firmware response */
	if (is_midr_in_range_list(read_cpuid_id(), entry->midr_range_list))
		this_cpu_safe = true;

	if (psci_ops.smccc_version == SMCCC_VERSION_1_0) {
		ssbd_state = ARM64_SSBD_UNKNOWN;
		if (!this_cpu_safe)
			__ssb_safe = false;
		return false;
	}

	switch (psci_ops.conduit) {
	case PSCI_CONDUIT_HVC:
		arm_smccc_1_1_hvc(ARM_SMCCC_ARCH_FEATURES_FUNC_ID,
				  ARM_SMCCC_ARCH_WORKAROUND_2, &res);
		break;

	case PSCI_CONDUIT_SMC:
		arm_smccc_1_1_smc(ARM_SMCCC_ARCH_FEATURES_FUNC_ID,
				  ARM_SMCCC_ARCH_WORKAROUND_2, &res);
		break;

	default:
		ssbd_state = ARM64_SSBD_UNKNOWN;
		if (!this_cpu_safe)
			__ssb_safe = false;
		return false;
	}

	val = (s32)res.a0;

	switch (val) {
	case SMCCC_RET_NOT_SUPPORTED:
		ssbd_state = ARM64_SSBD_UNKNOWN;
		if (!this_cpu_safe)
			__ssb_safe = false;
		return false;

	/* machines with mixed mitigation requirements must not return this */
	case SMCCC_RET_NOT_REQUIRED:
		pr_info_once("%s mitigation not required\n", entry->desc);
		ssbd_state = ARM64_SSBD_MITIGATED;
		return false;

	case SMCCC_RET_SUCCESS:
		__ssb_safe = false;
		required = true;
		break;

	case 1:	/* Mitigation not required on this CPU */
		required = false;
		break;

	default:
		WARN_ON(1);
		if (!this_cpu_safe)
			__ssb_safe = false;
		return false;
	}

	switch (ssbd_state) {
	case ARM64_SSBD_FORCE_DISABLE:
		pr_info_once("%s disabled from command-line\n", entry->desc);
		arm64_set_ssbd_mitigation(false);
		required = false;
		break;

	case ARM64_SSBD_KERNEL:
		if (required) {
			__this_cpu_write(arm64_ssbd_callback_required, 1);
			arm64_set_ssbd_mitigation(true);
		}
		break;

	case ARM64_SSBD_FORCE_ENABLE:
		pr_info_once("%s forced from command-line\n", entry->desc);
		arm64_set_ssbd_mitigation(true);
		required = true;
		break;

	default:
		WARN_ON(1);
		break;
	}

	return required;
}

/* known invulnerable cores */
static const struct midr_range arm64_ssb_cpus[] = {
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A35),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A53),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A55),
	{},
};

#define CAP_MIDR_RANGE(model, v_min, r_min, v_max, r_max)	\
	.matches = is_affected_midr_range,			\
	.midr_range = MIDR_RANGE(model, v_min, r_min, v_max, r_max)

#define CAP_MIDR_ALL_VERSIONS(model)					\
	.matches = is_affected_midr_range,				\
	.midr_range = MIDR_ALL_VERSIONS(model)

#define MIDR_FIXED(rev, revidr_mask) \
	.fixed_revs = (struct arm64_midr_revidr[]){{ (rev), (revidr_mask) }, {}}

#define ERRATA_MIDR_RANGE(model, v_min, r_min, v_max, r_max)		\
	.type = ARM64_CPUCAP_LOCAL_CPU_ERRATUM,				\
	CAP_MIDR_RANGE(model, v_min, r_min, v_max, r_max)

#define CAP_MIDR_RANGE_LIST(list)				\
	.matches = is_affected_midr_range_list,			\
	.midr_range_list = list

/* Errata affecting a range of revisions of  given model variant */
#define ERRATA_MIDR_REV_RANGE(m, var, r_min, r_max)	 \
	ERRATA_MIDR_RANGE(m, var, r_min, var, r_max)

/* Errata affecting a single variant/revision of a model */
#define ERRATA_MIDR_REV(model, var, rev)	\
	ERRATA_MIDR_RANGE(model, var, rev, var, rev)

/* Errata affecting all variants/revisions of a given a model */
#define ERRATA_MIDR_ALL_VERSIONS(model)				\
	.type = ARM64_CPUCAP_LOCAL_CPU_ERRATUM,			\
	CAP_MIDR_ALL_VERSIONS(model)

/* Errata affecting a list of midr ranges, with same work around */
#define ERRATA_MIDR_RANGE_LIST(midr_list)			\
	.type = ARM64_CPUCAP_LOCAL_CPU_ERRATUM,			\
	CAP_MIDR_RANGE_LIST(midr_list)

/* Track overall mitigation state. We are only mitigated if all cores are ok */
static bool __hardenbp_enab = true;
static bool __spectrev2_safe = true;

/*
 * List of CPUs that do not need any Spectre-v2 mitigation at all.
 */
static const struct midr_range spectre_v2_safe_list[] = {
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A35),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A53),
	MIDR_ALL_VERSIONS(MIDR_CORTEX_A55),
	{ /* sentinel */ }
};

/*
 * Track overall bp hardening for all heterogeneous cores in the machine.
 * We are only considered "safe" if all booted cores are known safe.
 */
static bool __maybe_unused
check_branch_predictor(const struct arm64_cpu_capabilities *entry, int scope)
{
	int need_wa;

	WARN_ON(scope != SCOPE_LOCAL_CPU || preemptible());

	/* If the CPU has CSV2 set, we're safe */
	if (cpuid_feature_extract_unsigned_field(read_cpuid(ID_AA64PFR0_EL1),
						 ID_AA64PFR0_CSV2_SHIFT))
		return false;

	/* Alternatively, we have a list of unaffected CPUs */
	if (is_midr_in_range_list(read_cpuid_id(), spectre_v2_safe_list))
		return false;

	/* Fallback to firmware detection */
	need_wa = detect_harden_bp_fw();
	if (!need_wa)
		return false;

	__spectrev2_safe = false;

	if (!IS_ENABLED(CONFIG_HARDEN_BRANCH_PREDICTOR)) {
		pr_warn_once("spectrev2 mitigation disabled by kernel configuration\n");
		__hardenbp_enab = false;
		return false;
	}

	/* forced off */
	if (__nospectre_v2 || cpu_mitigations_off()) {
		pr_info_once("spectrev2 mitigation disabled by command line option\n");
		__hardenbp_enab = false;
		return false;
	}

	if (need_wa < 0) {
		pr_warn_once("ARM_SMCCC_ARCH_WORKAROUND_1 missing from firmware\n");
		__hardenbp_enab = false;
	}

	return (need_wa > 0);
}

const struct arm64_cpu_capabilities arm64_errata[] = {
#if	defined(CONFIG_ARM64_ERRATUM_826319) || \
	defined(CONFIG_ARM64_ERRATUM_827319) || \
	defined(CONFIG_ARM64_ERRATUM_824069)
	{
	/* Cortex-A53 r0p[012] */
		.desc = "ARM errata 826319, 827319, 824069",
		.capability = ARM64_WORKAROUND_CLEAN_CACHE,
		ERRATA_MIDR_REV_RANGE(MIDR_CORTEX_A53, 0, 0, 2),
		.cpu_enable = cpu_enable_cache_maint_trap,
	},
#endif
#ifdef CONFIG_ARM64_ERRATUM_819472
	{
	/* Cortex-A53 r0p[01] */
		.desc = "ARM errata 819472",
		.capability = ARM64_WORKAROUND_CLEAN_CACHE,
		ERRATA_MIDR_REV_RANGE(MIDR_CORTEX_A53, 0, 0, 1),
		.cpu_enable = cpu_enable_cache_maint_trap,
	},
#endif
#ifdef CONFIG_ARM64_ERRATUM_832075
	{
	/* Cortex-A57 r0p0 - r1p2 */
		.desc = "ARM erratum 832075",
		.capability = ARM64_WORKAROUND_DEVICE_LOAD_ACQUIRE,
		ERRATA_MIDR_RANGE(MIDR_CORTEX_A57,
				  0, 0,
				  1, 2),
	},
#endif
#ifdef CONFIG_ARM64_ERRATUM_834220
	{
	/* Cortex-A57 r0p0 - r1p2 */
		.desc = "ARM erratum 834220",
		.capability = ARM64_WORKAROUND_834220,
		ERRATA_MIDR_RANGE(MIDR_CORTEX_A57,
				  0, 0,
				  1, 2),
	},
#endif
#ifdef CONFIG_ARM64_ERRATUM_845719
	{
	/* Cortex-A53 r0p[01234] */
		.desc = "ARM erratum 845719",
		.capability = ARM64_WORKAROUND_845719,
		ERRATA_MIDR_REV_RANGE(MIDR_CORTEX_A53, 0, 0, 4),
	},
#endif
#ifdef CONFIG_CAVIUM_ERRATUM_23154
	{
	/* Cavium ThunderX, pass 1.x */
		.desc = "Cavium erratum 23154",
		.capability = ARM64_WORKAROUND_CAVIUM_23154,
		ERRATA_MIDR_REV_RANGE(MIDR_THUNDERX, 0, 0, 1),
	},
#endif
#ifdef CONFIG_CAVIUM_ERRATUM_27456
	{
	/* Cavium ThunderX, T88 pass 1.x - 2.1 */
		.desc = "Cavium erratum 27456",
		.capability = ARM64_WORKAROUND_CAVIUM_27456,
		ERRATA_MIDR_RANGE(MIDR_THUNDERX,
				  0, 0,
				  1, 1),
	},
	{
	/* Cavium ThunderX, T81 pass 1.0 */
		.desc = "Cavium erratum 27456",
		.capability = ARM64_WORKAROUND_CAVIUM_27456,
		ERRATA_MIDR_REV(MIDR_THUNDERX_81XX, 0, 0),
	},
#endif
	{
		.desc = "Mismatched cache line size",
		.capability = ARM64_MISMATCHED_CACHE_LINE_SIZE,
		.matches = has_mismatched_cache_type,
		.type = ARM64_CPUCAP_LOCAL_CPU_ERRATUM,
		.cpu_enable = cpu_enable_trap_ctr_access,
	},
	{
		.desc = "Mismatched cache type",
		.capability = ARM64_MISMATCHED_CACHE_TYPE,
		.matches = has_mismatched_cache_type,
		.type = ARM64_CPUCAP_LOCAL_CPU_ERRATUM,
		.cpu_enable = cpu_enable_trap_ctr_access,
	},
	{
		.capability = ARM64_HARDEN_BRANCH_PREDICTOR,
		.type = ARM64_CPUCAP_LOCAL_CPU_ERRATUM,
		.matches = check_branch_predictor,
	},
	{
		.desc = "Speculative Store Bypass Disable",
		.type = ARM64_CPUCAP_LOCAL_CPU_ERRATUM,
		.capability = ARM64_SSBD,
		.matches = has_ssbd_mitigation,
		.midr_range_list = arm64_ssb_cpus,
	},
	{
	}
};

ssize_t cpu_show_spectre_v1(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	return sprintf(buf, "Mitigation: __user pointer sanitization\n");
}

ssize_t cpu_show_spectre_v2(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	if (__spectrev2_safe)
		return sprintf(buf, "Not affected\n");

	if (__hardenbp_enab)
		return sprintf(buf, "Mitigation: Branch predictor hardening\n");

	return sprintf(buf, "Vulnerable\n");
}

ssize_t cpu_show_spec_store_bypass(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	if (__ssb_safe)
		return sprintf(buf, "Not affected\n");

	switch (ssbd_state) {
	case ARM64_SSBD_KERNEL:
	case ARM64_SSBD_FORCE_ENABLE:
		if (IS_ENABLED(CONFIG_ARM64_SSBD))
			return sprintf(buf,
			    "Mitigation: Speculative Store Bypass disabled via prctl\n");
	}

	return sprintf(buf, "Vulnerable\n");
}
