// SPDX-License-Identifier: GPL-2.0-only
/*
 * Native, built-in PathMask for Sultan's integrated-modules configuration.
 * Inode permission, getattr and getdents hooks are at their VFS call sites.
 * No loadable module, kretprobe, or upstream runtime is required.
 *
 * Safe by default: no hidden paths, disabled, deny-scope, root/system exempt.
 * This is a native reimplementation of PathMask semantics, not a verbatim
 * port of the upstream LKM-PathMask code or its optional ProcGuard module.
 */
#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kdev_t.h>
#include <linux/kernel.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/pathmask.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>

#define PM_PATHS 64
#define PM_UIDS 256
#define PM_PATH_LEN 256
#define PM_CMD_LEN 320

enum pm_mode { PM_DENY, PM_ALLOW, PM_GLOBAL };

struct pm_target {
	dev_t dev;
	unsigned long ino;
	char name[PM_PATH_LEN];
};

static DEFINE_SPINLOCK(pm_lock);
static struct pm_target pm_targets[PM_PATHS];
static u32 pm_denied[PM_UIDS];
static u32 pm_allowed[PM_UIDS];
static unsigned int pm_npaths, pm_ndeny, pm_nallow;
static enum pm_mode pm_scope = PM_DENY;
static bool pm_enabled;
static bool pm_dirents = true;

static bool pm_uid_has(u32 *ids, unsigned int count, u32 uid)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		if (ids[i] == uid)
			return true;
	return false;
}

/* Must hold pm_lock. */
static bool pm_uid_in_scope(u32 uid)
{
	if (uid == 0 || uid == 1000)
		return false;
	switch (pm_scope) {
	case PM_DENY:
		return pm_uid_has(pm_denied, pm_ndeny, uid);
	case PM_ALLOW:
		return !pm_uid_has(pm_allowed, pm_nallow, uid);
	case PM_GLOBAL:
		return true;
	}
	return false;
}

static bool pm_matches(dev_t dev, u64 ino, bool from_dirent)
{
	unsigned long flags;
	unsigned int i;
	u32 uid;
	bool result = false;

	if (likely(!READ_ONCE(pm_enabled)) || !ino)
		return false;
	if (from_dirent && !READ_ONCE(pm_dirents))
		return false;

	uid = __kuid_val(current_uid());
	if (uid == 0 || uid == 1000)
		return false;

	spin_lock_irqsave(&pm_lock, flags);
	if (pm_enabled && (!from_dirent || pm_dirents) &&
	    pm_uid_in_scope(uid)) {
		for (i = 0; i < pm_npaths; i++) {
			if (pm_targets[i].dev == dev && pm_targets[i].ino == ino) {
				result = true;
				break;
			}
		}
	}
	spin_unlock_irqrestore(&pm_lock, flags);
	return result;
}

bool pathmask_should_hide_inode(const struct inode *inode)
{
	return inode && pm_matches(inode->i_sb->s_dev, inode->i_ino, false);
}

bool pathmask_should_hide_dirent(dev_t dev, u64 ino)
{
	return pm_matches(dev, ino, true);
}

/* Avoid accidentally breaking Android Zygote and vendor binaries. */
static bool pm_is_critical(const char *name)
{
	static const char * const prefixes[] = {
		"/system/framework", "/system/bin", "/system/lib",
		"/system_ext/framework", "/product/framework", "/apex",
		"/vendor/bin", "/vendor/lib"
	};
	unsigned int i;

	if (!strcmp(name, "/"))
		return true;
	for (i = 0; i < ARRAY_SIZE(prefixes); i++) {
		size_t len = strlen(prefixes[i]);

		if (!strncmp(name, prefixes[i], len) &&
		    (name[len] == '/' || name[len] == '\0'))
			return true;
	}
	return false;
}

static int pm_add(const char *name)
{
	struct path p;
	struct inode *inode;
	unsigned int i;
	unsigned long flags;
	int ret;

	if (name[0] != '/' || strlen(name) >= PM_PATH_LEN)
		return -EINVAL;
	if (pm_is_critical(name))
		return -EPERM;
	ret = kern_path(name, LOOKUP_FOLLOW, &p);
	if (ret)
		return ret;
	inode = d_inode(p.dentry);
	if (!inode) {
		path_put(&p);
		return -ENOENT;
	}

	spin_lock_irqsave(&pm_lock, flags);
	for (i = 0; i < pm_npaths; i++) {
		if (!strcmp(pm_targets[i].name, name))
			break;
	}
	if (i == PM_PATHS) {
		ret = -ENOSPC;
		goto out;
	}
	if (i == pm_npaths) {
		if (pm_npaths == PM_PATHS) {
			ret = -ENOSPC;
			goto out;
		}
		pm_npaths++;
	}
	pm_targets[i].dev = inode->i_sb->s_dev;
	pm_targets[i].ino = inode->i_ino;
	strscpy(pm_targets[i].name, name, PM_PATH_LEN);
out:
	spin_unlock_irqrestore(&pm_lock, flags);
	path_put(&p);
	return ret;
}

static int pm_remove(const char *name)
{
	unsigned int i;
	unsigned long flags;
	int ret = -ENOENT;

	spin_lock_irqsave(&pm_lock, flags);
	for (i = 0; i < pm_npaths; i++) {
		if (!strcmp(pm_targets[i].name, name)) {
			pm_targets[i] = pm_targets[--pm_npaths];
			ret = 0;
			break;
		}
	}
	spin_unlock_irqrestore(&pm_lock, flags);
	return ret;
}

static int pm_uid_change(const char *s, bool deny, bool remove)
{
	unsigned int id, i, *count;
	unsigned long flags;
	u32 *list;
	int ret = 0;

	if (kstrtouint(s, 10, &id))
		return -EINVAL;
	spin_lock_irqsave(&pm_lock, flags);
	list = deny ? pm_denied : pm_allowed;
	count = deny ? &pm_ndeny : &pm_nallow;
	for (i = 0; i < *count; i++) {
		if (list[i] == id)
			break;
	}
	if (remove) {
		if (i == *count)
			ret = -ENOENT;
		else
			list[i] = list[--*count];
	} else if (i == *count) {
		if (*count == PM_UIDS)
			ret = -ENOSPC;
		else
			list[(*count)++] = id;
	}
	spin_unlock_irqrestore(&pm_lock, flags);
	return ret;
}

static int pm_show(struct seq_file *m, void *v)
{
	unsigned long flags;
	unsigned int i, paths, deny, allow;
	enum pm_mode mode;
	struct pm_target path;
	u32 uid;
	bool active, dirents;

	spin_lock_irqsave(&pm_lock, flags);
	active = pm_enabled;
	dirents = pm_dirents;
	mode = pm_scope;
	paths = pm_npaths;
	deny = pm_ndeny;
	allow = pm_nallow;
	spin_unlock_irqrestore(&pm_lock, flags);

	seq_printf(m, "enabled=%u\nmode=%s\ndirents=%u\n", active,
		   mode == PM_DENY ? "deny" :
		   mode == PM_ALLOW ? "allow" : "global", dirents);
	for (i = 0; i < paths; i++) {
		spin_lock_irqsave(&pm_lock, flags);
		path = pm_targets[i];
		spin_unlock_irqrestore(&pm_lock, flags);
		seq_printf(m, "path=%s dev=%u:%u ino=%lu\n", path.name,
			   MAJOR(path.dev), MINOR(path.dev), path.ino);
	}
	for (i = 0; i < deny; i++) {
		spin_lock_irqsave(&pm_lock, flags);
		uid = pm_denied[i];
		spin_unlock_irqrestore(&pm_lock, flags);
		seq_printf(m, "deny_uid=%u\n", uid);
	}
	for (i = 0; i < allow; i++) {
		spin_lock_irqsave(&pm_lock, flags);
		uid = pm_allowed[i];
		spin_unlock_irqrestore(&pm_lock, flags);
		seq_printf(m, "allow_uid=%u\n", uid);
	}
	return 0;
}

static int pm_open(struct inode *inode, struct file *file)
{
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	return single_open(file, pm_show, NULL);
}

static ssize_t pm_write(struct file *file, const char __user *ubuf,
			size_t length, loff_t *offset)
{
	char *buf, *op, *arg;
	unsigned long flags;
	enum pm_mode mode;
	bool enabled;
	int ret = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (!length || length > PM_CMD_LEN)
		return -EINVAL;
	buf = memdup_user_nul(ubuf, length);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	op = strim(buf);
	arg = strpbrk(op, " \t");
	if (arg) {
		*arg++ = '\0';
		arg = strim(arg);
	} else {
		arg = op + strlen(op);
	}

	if (!strcmp(op, "add"))
		ret = pm_add(arg);
	else if (!strcmp(op, "del"))
		ret = pm_remove(arg);
	else if (!strcmp(op, "deny_uid"))
		ret = pm_uid_change(arg, true, false);
	else if (!strcmp(op, "allow_uid"))
		ret = pm_uid_change(arg, false, false);
	else if (!strcmp(op, "remove_deny_uid"))
		ret = pm_uid_change(arg, true, true);
	else if (!strcmp(op, "remove_allow_uid"))
		ret = pm_uid_change(arg, false, true);
	else if (!strcmp(op, "clear")) {
		spin_lock_irqsave(&pm_lock, flags);
		pm_enabled = false;
		pm_npaths = pm_ndeny = pm_nallow = 0;
		pm_scope = PM_DENY;
		pm_dirents = true;
		spin_unlock_irqrestore(&pm_lock, flags);
	} else if (!strcmp(op, "mode")) {
		if (!strcmp(arg, "deny"))
			mode = PM_DENY;
		else if (!strcmp(arg, "allow"))
			mode = PM_ALLOW;
		else if (!strcmp(arg, "global"))
			mode = PM_GLOBAL;
		else
			ret = -EINVAL;
		if (!ret) {
			spin_lock_irqsave(&pm_lock, flags);
			pm_scope = mode;
			spin_unlock_irqrestore(&pm_lock, flags);
		}
	} else if (!strcmp(op, "enable") || !strcmp(op, "dirents")) {
		ret = kstrtobool(arg, &enabled);
		if (!ret) {
			spin_lock_irqsave(&pm_lock, flags);
			if (!strcmp(op, "enable"))
				pm_enabled = enabled;
			else
				pm_dirents = enabled;
			spin_unlock_irqrestore(&pm_lock, flags);
		}
	} else {
		ret = -EINVAL;
	}
	kfree(buf);
	return ret ? ret : length;
}

static const struct proc_ops pm_ops = {
	.proc_open = pm_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
	.proc_write = pm_write,
};

static int __init pm_init(void)
{
	if (!proc_create("pathmask_ctl", 0600, NULL, &pm_ops)) {
		pr_err("pathmask: failed to register control\n");
		return -ENOMEM;
	}
	pr_info("pathmask: VFS built-in ready (disabled by default)\n");
	return 0;
}
late_initcall(pm_init);
