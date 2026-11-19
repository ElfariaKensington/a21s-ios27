#ifndef _LX_LINUX_PROC_FS_H_
#define _LX_LINUX_PROC_FS_H_

#include <linux/fs.h>

#ifndef _KUID_T_DEFINED_LX
#define _KUID_T_DEFINED_LX
typedef uid_t kuid_t;
typedef gid_t kgid_t;
#define KUIDT_INIT(v)	((kuid_t)(v))
#define KGIDT_INIT(v)	((kgid_t)(v))
#endif

struct proc_dir_entry {
	void *data;
};

static inline struct proc_dir_entry *
proc_mkdir(const char *name, struct proc_dir_entry *parent)
{
	(void)name;
	(void)parent;
	return (NULL);
}

static inline struct proc_dir_entry *
proc_create_data(const char *name, umode_t mode, struct proc_dir_entry *parent,
    const void *proc_fops, void *data)
{
	(void)name;
	(void)mode;
	(void)parent;
	(void)proc_fops;
	(void)data;
	return (NULL);
}

static inline void
remove_proc_entry(const char *name, struct proc_dir_entry *parent)
{
	(void)name;
	(void)parent;
}

static inline void *
PDE_DATA(const struct inode *inode)
{
	(void)inode;
	return (NULL);
}

static inline void
proc_set_user(struct proc_dir_entry *de, kuid_t uid, kgid_t gid)
{
	(void)de;
	(void)uid;
	(void)gid;
}

#endif
