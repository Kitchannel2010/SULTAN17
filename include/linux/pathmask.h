/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_PATHMASK_BUILTIN_H
#define _LINUX_PATHMASK_BUILTIN_H

#include <linux/fs.h>
#include <linux/types.h>

#ifdef CONFIG_PATHMASK
bool pathmask_should_hide_inode(const struct inode *inode);
bool pathmask_should_hide_dirent(dev_t device, u64 inode_number);
#else
static inline bool pathmask_should_hide_inode(const struct inode *inode)
{
	return false;
}
static inline bool pathmask_should_hide_dirent(dev_t device, u64 inode_number)
{
	return false;
}
#endif
#endif
