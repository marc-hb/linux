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


dmr-6.14-v3.2
---------------
13. fix: APX feature bits CPUID in guest is not correct
    https://jira.devtools.intel.com/browse/LFE-16870

dmr-6.14-v3.1
---------------
12. fix: OS doesn't understand CXL CPER log for CXL protocol errors and event
    logs. https://jira.devtools.intel.com/browse/LINUXBKC3-215

dmr-6.14-v2.6
---------------
11. fix: KVM kselftest monitor_mwait_test fail
    https://jira.devtools.intel.com/browse/LFE-16865

dmr-6.14-v2.5
---------------
10. Add new Linux OS kernel boot parameter iommu=ignore [ intel_iommu=ignore ]
    https://jira.devtools.intel.com/browse/LFE-16161

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

dmr-6.14-v2.1
---------------
5.   update from 6.14-rc7 to 6.14

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

