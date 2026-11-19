#ifndef _LX_EXYNOS_SOC_H_
#define _LX_EXYNOS_SOC_H_

#include <linux/types.h>

struct exynos_chipid_info {
	u32 product_id;
	u32 revision;
};

extern struct exynos_chipid_info exynos_soc_info;

#endif
