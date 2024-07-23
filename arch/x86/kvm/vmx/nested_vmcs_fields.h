#if !defined(HAS_VMCS_FIELD) && !defined(HAS_VMCS_FIELD_RANGE)
BUILD_BUG_ON(1)
#endif

#ifndef HAS_VMCS_FIELD
#define HAS_VMCS_FIELD(x, c)
#endif
#ifndef HAS_VMCS_FIELD_RANGE
#define HAS_VMCS_FIELD_RANGE(x, y, c)
#endif

#undef HAS_VMCS_FIELD
#undef HAS_VMCS_FIELD_RANGE
