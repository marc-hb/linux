// SPDX-License-Identifier: GPL-2.0-only
/*
 * avx10 tests
 *
 * Copyright (C) 2024, Intel, Inc.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>

#include "test_util.h"

#include "kvm_util.h"
#include "processor.h"
#include "vmx.h"

static void init_regs(void)
{
	uint64_t cr4, xcr0;

	GUEST_ASSERT(this_cpu_has(X86_FEATURE_XSAVE));

	/* turn on CR4.OSXSAVE */
	cr4 = get_cr4();
	cr4 |= X86_CR4_OSXSAVE;
	set_cr4(cr4);
	GUEST_ASSERT(this_cpu_has(X86_FEATURE_OSXSAVE));

	xcr0 = xgetbv(0);
	xcr0 |= XFEATURE_MASK_AVX512 | XFEATURE_MASK_SSE | XFEATURE_MASK_YMM;
	xsetbv(0x0, xcr0);

	xcr0 = xgetbv(0);
}

void guest_ud_handler(struct ex_regs *regs)
{
	GUEST_SYNC(4);
	GUEST_DONE();
}

static void __attribute__((__flatten__)) guest_code(void)
{
	/* Check guest cpuid */
	GUEST_ASSERT(this_cpu_has(X86_FEATURE_AVX10));
	GUEST_ASSERT(this_cpu_has(X86_FEATURE_AVX10_256));
	GUEST_ASSERT(!this_cpu_has(X86_FEATURE_AVX10_512));
	GUEST_SYNC(1);

	init_regs();
	GUEST_SYNC(2);

	/* Run AVX512(VL=256) instruction */
	asm volatile("vpxord %ymm0, %ymm0, %ymm0");
	GUEST_SYNC(3);

	/* Run AVX512(VL=512) instruction */
	asm volatile("vpxord %zmm0, %zmm0, %zmm0");
	GUEST_SYNC(5);
	GUEST_DONE();
}

int main(int argc, char *argv[])
{
	struct kvm_regs regs1, regs2;
	struct kvm_vm *vm;
	struct kvm_vcpu *vcpu;
	struct kvm_x86_state *state;
	struct ucall uc;
	int stage;

	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_XSAVE));
	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_OPMASK));
	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_ZMM_HI256));
	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_HI16_ZMM));
	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_AVX10));
	TEST_REQUIRE(kvm_cpu_has(X86_FEATURE_AVX10_256));

	/* Create VM */
	vm = vm_create_with_one_vcpu(&vcpu, guest_code);

	/* Set VCPU to AVX10/256 bits */
	if (kvm_cpu_has(X86_FEATURE_AVX10_512))
		vcpu_clear_cpuid_feature(vcpu, X86_FEATURE_AVX10_512);

	vcpu_regs_get(vcpu, &regs1);

	/* Register #UD handler */
	vm_install_exception_handler(vm, UD_VECTOR, guest_ud_handler);

	for (stage = 1; ; stage++) {
		vcpu_run(vcpu);
		TEST_ASSERT_KVM_EXIT_REASON(vcpu, KVM_EXIT_IO);

		switch (get_ucall(vcpu, &uc)) {
		case UCALL_ABORT:
			REPORT_GUEST_ASSERT(uc);
			/* NOT REACHED */
		case UCALL_SYNC:
			switch (uc.args[1]) {
			case 1:
			case 2:
			case 3:
			case 4:
				fprintf(stderr, "GUEST_SYNC(%ld)\n", uc.args[1]);
				break;
			case 5:
				if (kvm_cpu_property(X86_PROPERTY_AVX10_VERSION) >= 2)
					TEST_FAIL("No #UD when executing VL=512 instruction in "
						  "AVX10.2/256 bits guest");
				fprintf(stderr, "GUEST_SYNC(%ld)\n", uc.args[1]);
				break;
			default:
				fprintf(stderr, "GUEST_SYNC(%ld)\n", uc.args[1]);
				break;
			}
			break;
		case UCALL_DONE:
			fprintf(stderr, "UCALL_DONE\n");
			goto done;
		default:
			TEST_FAIL("Unknown ucall %lu", uc.cmd);
		}

		state = vcpu_save_state(vcpu);
		memset(&regs1, 0, sizeof(regs1));
		vcpu_regs_get(vcpu, &regs1);

		kvm_vm_release(vm);

		/* Restore state in a new VM.  */
		vcpu = vm_recreate_with_one_vcpu(vm);
		/* Set VCPU to AVX10/256 bits */
		if (kvm_cpu_has(X86_FEATURE_AVX10_512))
			vcpu_clear_cpuid_feature(vcpu, X86_FEATURE_AVX10_512);
		vcpu_load_state(vcpu, state);
		kvm_x86_state_cleanup(state);

		memset(&regs2, 0, sizeof(regs2));
		vcpu_regs_get(vcpu, &regs2);
		TEST_ASSERT(!memcmp(&regs1, &regs2, sizeof(regs2)),
			    "Unexpected register values after vcpu_load_state; rdi: %lx rsi: %lx",
			    (ulong) regs2.rdi, (ulong) regs2.rsi);
	}

done:
	kvm_vm_free(vm);
}
