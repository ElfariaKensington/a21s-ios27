#ifndef _LINUXKPI_NET_NETLINK_H
#define _LINUXKPI_NET_NETLINK_H

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/skbuff.h>
#include <netlink/netlink.h>

enum {
	NLA_UNSPEC,
	NLA_U8,
	NLA_U16,
	NLA_U32,
	NLA_U64,
	NLA_STRING,
	NLA_FLAG,
	NLA_MSECS,
	NLA_NESTED,
	NLA_NESTED_ARRAY,
	NLA_NUL_STRING,
	NLA_BINARY,
	NLA_S8,
	NLA_S16,
	NLA_S32,
	NLA_S64,
};

struct nla_policy {
	uint16_t type;
	uint16_t len;
	uint16_t maxlen;
};

struct netlink_callback {
	int args[8];
};

#ifndef NLA_HDRLEN
#define NLA_HDRLEN	((int)sizeof(struct nlattr))
#endif

static inline int
nla_type(const struct nlattr *nla)
{
	return (nla->nla_type & 0x3fff);
}

static inline int
nla_len(const struct nlattr *nla)
{
	return ((int)nla->nla_len - NLA_HDRLEN);
}

static inline void *
nla_data(const struct nlattr *nla)
{
	return ((char *)(uintptr_t)nla + NLA_HDRLEN);
}

static inline int
nla_ok(const struct nlattr *nla, int remaining)
{
	return (remaining >= (int)sizeof(*nla) &&
	    nla->nla_len >= sizeof(*nla) &&
	    nla->nla_len <= remaining);
}

static inline struct nlattr *
nla_next(const struct nlattr *nla, int *remaining)
{
	int totlen = ((nla->nla_len + 3) & ~3);

	*remaining -= totlen;
	return ((struct nlattr *)((char *)(uintptr_t)nla + totlen));
}

#ifndef nla_for_each_attr
#define nla_for_each_attr(pos, head, len, rem) \
	for (pos = head, rem = len; nla_ok(pos, rem); pos = nla_next(pos, &(rem)))
#endif

#ifndef nla_for_each_nested
#define nla_for_each_nested(pos, nla, rem) \
	nla_for_each_attr(pos, (struct nlattr *)nla_data(nla), nla_len(nla), rem)
#endif

#define _LX_NLA_GET(name, rtype) \
static inline rtype name(const struct nlattr *nla) \
{ \
	rtype v = 0; \
	__builtin_memcpy(&v, nla_data(nla), sizeof(v)); \
	return (v); \
}

_LX_NLA_GET(nla_get_u8, u8)
_LX_NLA_GET(nla_get_u16, u16)
_LX_NLA_GET(nla_get_u32, u32)
_LX_NLA_GET(nla_get_u64, u64)
_LX_NLA_GET(nla_get_s8, s8)
_LX_NLA_GET(nla_get_s16, s16)
_LX_NLA_GET(nla_get_s32, s32)
_LX_NLA_GET(nla_get_s64, s64)
_LX_NLA_GET(nla_get_be16, __be16)
_LX_NLA_GET(nla_get_be32, __be32)
_LX_NLA_GET(nla_get_be64, __be64)
_LX_NLA_GET(nla_get_le16, __le16)
_LX_NLA_GET(nla_get_le32, __le32)
_LX_NLA_GET(nla_get_le64, __le64)

static inline int
nla_put(struct sk_buff *skb, int attr, size_t len, const void *data)
{
	(void)skb;
	(void)attr;
	(void)len;
	(void)data;
	return (-ENXIO);
}

static inline int nla_put_u8(struct sk_buff *skb, int attr, u8 val) { return nla_put(skb, attr, sizeof(val), &val); }
static inline int nla_put_u16(struct sk_buff *skb, int attr, u16 val) { return nla_put(skb, attr, sizeof(val), &val); }
static inline int nla_put_u32(struct sk_buff *skb, int attr, u32 val) { return nla_put(skb, attr, sizeof(val), &val); }
static inline int nla_put_u64_64bit(struct sk_buff *skb, int attr, u64 val, int padattr) { (void)padattr; return nla_put(skb, attr, sizeof(val), &val); }
static inline int nla_put_s8(struct sk_buff *skb, int attr, s8 val) { return nla_put(skb, attr, sizeof(val), &val); }
static inline int nla_put_s16(struct sk_buff *skb, int attr, s16 val) { return nla_put(skb, attr, sizeof(val), &val); }
static inline int nla_put_s32(struct sk_buff *skb, int attr, s32 val) { return nla_put(skb, attr, sizeof(val), &val); }
static inline int nla_put_string(struct sk_buff *skb, int attr, const char *str) { return nla_put(skb, attr, __builtin_strlen(str) + 1, str); }
static inline int nla_put_flag(struct sk_buff *skb, int attr) { return nla_put(skb, attr, 0, NULL); }
static inline struct nlattr *nla_nest_start(struct sk_buff *skb, int attr) { (void)skb; (void)attr; return (NULL); }
static inline int nla_nest_end(struct sk_buff *skb, struct nlattr *start) { (void)skb; (void)start; return (0); }
static inline void nla_nest_cancel(struct sk_buff *skb, struct nlattr *start) { (void)skb; (void)start; }

#endif
