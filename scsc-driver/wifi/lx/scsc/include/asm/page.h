#ifndef _LX_ASM_PAGE_H_
#define _LX_ASM_PAGE_H_

#include <sys/param.h>
#ifndef PAGE_ALIGN
#define PAGE_ALIGN(addr)	round_page(addr)
#endif

#endif
