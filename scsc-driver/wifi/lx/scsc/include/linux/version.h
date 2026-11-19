#ifndef _LX_LINUX_VERSION_H_
#define _LX_LINUX_VERSION_H_

#ifndef KERNEL_VERSION
#define KERNEL_VERSION(a, b, c)		(((a) << 16) + ((b) << 8) + ((c) > 255 ? 255 : (c)))
#endif

#undef LINUX_VERSION_CODE
#define LINUX_VERSION_CODE		KERNEL_VERSION(5, 4, 0)

#endif
