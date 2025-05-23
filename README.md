Purpose
=======
Provide Best Known Configuration (BKC) kernel for DMR validation.

The BKC kernel is based on a set of intel-next PR's for the v6.12 kernel 

WARNING this kernel contains technology preview code that is
subject to change once it goes upstream. This kernel is
strictly for hardware validation, not production. Applications
tested against this kernel may behave differently, or may not
operate at all once the code is finalized in the mainline kernel.
Use at your own risk.

This code has NOT been scanned for IP or Security issues and is NOT elegable
for exporting outside of Intel.

Release History
===============


dmr-6.14-v5.3
---------------
27. fix: [CWF][CWF-BKC][kselftest][vPMU]pmu_counters_test failed
    https://jira.devtools.intel.com/browse/LFE-16934

dmr-6.14-v5.2
---------------
27. fix: [CWF-BKC] ddt lass test case : CPU_XS_FUNC_LASS_VTIME_EMULATION fails
    to complete with FRED enabled
    https://jira.devtools.intel.com/browse/LFE-16932

dmr-6.14-v5.1
---------------
26. fix: [DMR-Simics][DMR-BKC]AMX CPUID in guest is not correct
    https://jira.devtools.intel.com/browse/LFE-16869

dmr-6.14-v4.9
---------------
25. fix: Observed call trace during G3/S5/WR cycle  __warn+0x81/0x130 ?
    ast_dp_set_enable+0xfc/0x150 [ast]
    https://jira.devtools.intel.com/browse/LINUXBKC3-634
    https://hsdes.intel.com/appstore/article/#/16027295052

dmr-6.14-v4.8
---------------
24. fix: [Cluster][CTRLS CWF_AP A0][1S] Node hang with Kernel panic - during
    sandstone-rf-warm https://jira.devtools.intel.com/browse/LINUXBKC3-640

dmr-6.14-v4.7
---------------
23. fix: Invalid access to IO port in the TCO device for DMR
    https://jira.devtools.intel.com/browse/LINUXBKC3-562

dmr-6.14-v4.6
---------------
22. fixup: missed a change for v4.1 [gen2 trunk branch] No method to identify
    which S3M mailbox proxy corresponds to the Legacy CPU (for miniDPE use
    cases)
    https://hsdes.intel.com/appstore/article/#/14023586324
    https://jira.devtools.intel.com/browse/LINUXBKC3-646

dmr-6.14-v4.5
---------------
21. fix: [DMR-Simics][DMR-BKC]WRMSRNS is not exposed to guest
    https://jira.devtools.intel.com/browse/LFE-16874
    https://jira.devtools.intel.com/browse/LINUXBKC3-645

dmr-6.14-v4.4
---------------
20. fix: [CWF-BKC][kselftest][FRED] x86/sigreturn_64 test failure and
    segmentation fault in case of fred=on
    https://jira.devtools.intel.com/browse/LFE-16928
    https://jira.devtools.intel.com/browse/LINUXBKC3-639

dmr-6.14-v4.3
---------------
19. fix: [CWF][GNR][Intel-next] modprobe kvm_intel enable_mediated_pmu=Y will
    trigger Call trace
    https://jira.devtools.intel.com/browse/LFE-17122
    original ticket: https://jira.devtools.intel.com/browse/LFE-16891
    https://jira.devtools.intel.com/browse/LINUXBKC3-638

dmr-6.14-v4.2
---------------
18. fix: [DMR-Simics][DMR-BKC][PMU] Boot up VM with "unchecked MSR access
    error: WRMSR to 0x1a6" when mediate VPMU enabled
    https://jira.devtools.intel.com/browse/LFE-16891
    [DMR-Simics][DMR-BKC][PMU] Boot up VM with "unchecked MSR access error:
    WRMSR to 0x1a6" when mediate VPMU enabled
    https://jira.devtools.intel.com/browse/LINUXBKC3-636

dmr-6.14-v4.1
---------------
17. fix: [gen2 trunk branch] No method to identify which S3M mailbox proxy
    corresponds to the Legacy CPU (for miniDPE use cases)
    https://hsdes.intel.com/appstore/article/#/14023586324
    [gen2 trunk branch] No method to identify which S3M mailbox proxy
    corresponds to the Legacy CPU (for miniDPE use cases)
    https://jira.devtools.intel.com/browse/LINUXBKC3-632

dmr-6.14-v3.5
---------------
16. fix: OS kexec reboot fail with a Simics triple fault
    https://jira.devtools.intel.com/browse/LINUXBKC3-630

dmr-6.14-v3.4
---------------
15. fix: It fails to boot TD guest with fred=on on host, simics crash
    https://jira.devtools.intel.com/browse/LFE-16679
    https://jira.devtools.intel.com/browse/LINUXBKC3-574

dmr-6.14-v3.3
---------------
14. fix: System fails to undergo shutdown/S5 State when booted to tboot OS with
    TXT enable
    https://hsdes.intel.com/appstore/article/#/16027244517
    https://jira.devtools.intel.com/browse/LINUXBKC3-620

dmr-6.14-v3.2
---------------
13. fix: APX feature bits CPUID in guest is not correct
    https://jira.devtools.intel.com/browse/LFE-16870
    https://jira.devtools.intel.com/browse/LINUXBKC3-610

dmr-6.14-v3.1
---------------
12. fix: OS doesn't understand CXL CPER log for CXL protocol errors and event
    logs. https://jira.devtools.intel.com/browse/LINUXBKC3-215

dmr-6.14-v2.6
---------------
11. fix: KVM kselftest monitor_mwait_test fail
    https://jira.devtools.intel.com/browse/LFE-16865
    https://jira.devtools.intel.com/browse/LINUXBKC3-600

dmr-6.14-v2.5
---------------
10. Add new Linux OS kernel boot parameter iommu=ignore [ intel_iommu=ignore ]
    https://jira.devtools.intel.com/browse/LFE-16161
    https://jira.devtools.intel.com/browse/LINUXBKC3-591

dmr-6.14-v2.4
---------------
9.   fix: SST Tool output discrepancies on 2S Simics
     https://jira.devtools.intel.com/browse/LINUXBKC3-580 

dmr-6.14-v2.3
---------------
8.   fix TDX issue with simics using non-upstreamed patches.

dmr-6.14-v2.2
---------------
7.   update config options for QAT per QAT dev team.
6.   cosmedec: change mc to imc for uncore.
     https://jira.devtools.intel.com/browse/LFE-154
     https://jira.devtools.intel.com/browse/LINUXBKC3-594
     https://jira.devtools.intel.com/browse/LINUXBKC3-571

dmr-6.14-v2.1
---------------
5.   update from 6.14-rc7 to 6.14
     https://jira.devtools.intel.com/browse/LINUXBKC3-593

dmr-6.14-v1.0
---------------
4.   add arch/x86/configs/dmr.config file for build.

3.   Add initial README.md file

2.   Add do-it-dmr.sh script used for generating the BKC from intel-next.

1.   fix compile issues with oobmsm

Intel next branches included in v1.0:
------------------
./merge.py -w \
"rapl/idle,\
SPI-NOR/PCI/Thunderbolt/USB4,\
LPSS/SMBus,\
cpuid_dmr_nis,\
ifs_changes,\
pcie6,\
LASS,\
edac,\
edac_fixup,\
fred_kvm,\
fred_fixup,\
mm_region,\
KVM LASS,\
ras_bff_dmr,\
oobmsm,\
avx_10,\
sgx_sha384,\
dmr_apx,\
iommu_sats,\
perf,\
perf_fixup,\
perf_tdx_kvm_support_fixup,\
ucode_update,\
kvm_cet,\
nmi_source,\
msr_instructions,\
msr_instructions_fixup,\
tdx_kvm_support,\
tdx_kvm_support_fixup,\
dsa_batchop_fix,\
dsa3_dmr,\
qat_support,\
nmi_source_fixup"



initial features and bug fixes assocated Intel-next PRs for dmr-6.14-v1.0
=====================
https://jira.devtools.intel.com/browse/LINUXBKC3-578
https://jira.devtools.intel.com/browse/LINUXBKC3-572
https://jira.devtools.intel.com/browse/LINUXBKC3-533
https://jira.devtools.intel.com/browse/LINUXBKC3-532
https://jira.devtools.intel.com/browse/LINUXBKC3-531
https://jira.devtools.intel.com/browse/LINUXBKC3-530
https://jira.devtools.intel.com/browse/LINUXBKC3-529
https://jira.devtools.intel.com/browse/LINUXBKC3-522
https://jira.devtools.intel.com/browse/LINUXBKC3-464
https://jira.devtools.intel.com/browse/LINUXBKC3-452
https://jira.devtools.intel.com/browse/LINUXBKC3-442
https://jira.devtools.intel.com/browse/LINUXBKC3-437
https://jira.devtools.intel.com/browse/LINUXBKC3-425
https://jira.devtools.intel.com/browse/LINUXBKC3-424
https://jira.devtools.intel.com/browse/LINUXBKC3-410
https://jira.devtools.intel.com/browse/LINUXBKC3-409
https://jira.devtools.intel.com/browse/LINUXBKC3-334
https://jira.devtools.intel.com/browse/LINUXBKC3-330
https://jira.devtools.intel.com/browse/LINUXBKC3-329
https://jira.devtools.intel.com/browse/LINUXBKC3-328
https://jira.devtools.intel.com/browse/LINUXBKC3-326
https://jira.devtools.intel.com/browse/LINUXBKC3-325
https://jira.devtools.intel.com/browse/LINUXBKC3-323
https://jira.devtools.intel.com/browse/LINUXBKC3-322
https://jira.devtools.intel.com/browse/LINUXBKC3-321
https://jira.devtools.intel.com/browse/LINUXBKC3-320
https://jira.devtools.intel.com/browse/LINUXBKC3-319
https://jira.devtools.intel.com/browse/LINUXBKC3-318
https://jira.devtools.intel.com/browse/LINUXBKC3-317
https://jira.devtools.intel.com/browse/LINUXBKC3-316
https://jira.devtools.intel.com/browse/LINUXBKC3-315
https://jira.devtools.intel.com/browse/LINUXBKC3-314
https://jira.devtools.intel.com/browse/LINUXBKC3-309
https://jira.devtools.intel.com/browse/LINUXBKC3-305
https://jira.devtools.intel.com/browse/LINUXBKC3-299
https://jira.devtools.intel.com/browse/LINUXBKC3-298
https://jira.devtools.intel.com/browse/LINUXBKC3-286
https://jira.devtools.intel.com/browse/LINUXBKC3-277

