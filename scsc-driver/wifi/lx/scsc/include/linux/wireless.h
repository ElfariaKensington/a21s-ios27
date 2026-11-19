#ifndef _LX_LINUX_WIRELESS_H_
#define _LX_LINUX_WIRELESS_H_

#include <linux/types.h>
#include <linux/socket.h>

#ifndef IFNAMSIZ
#define IFNAMSIZ		16
#endif

#ifndef SIOCDEVPRIVATE
#define SIOCDEVPRIVATE		0x89F0
#endif

struct iw_point {
	void __user *pointer;
	__u16 length;
	__u16 flags;
};

union iwreq_data {
	char name[IFNAMSIZ];
	struct iw_point data;
};

struct iwreq {
	char ifr_name[IFNAMSIZ];
	union iwreq_data u;
};

struct iw_request_info {
	__u16 cmd;
	__u16 flags;
};

#endif
