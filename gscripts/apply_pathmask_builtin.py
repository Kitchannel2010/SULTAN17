#!/usr/bin/env python3
"""Inject native PathMask call sites after all third-party VFS patch hunks.

Do not add these call sites to the checked-in fs/*.c files: the SUSFS patch
set expects pristine upstream contexts, and partially rejected include hunks
otherwise generate uncompilable sources. Failure is fatal, not optional.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def one(text: str, before: str, after: str, label: str) -> str:
    occurrences = text.count(before)
    if occurrences != 1:
        raise SystemExit(f"PathMask: {label}: expected one anchor, got {occurrences}")
    return text.replace(before, after, 1)


def scoped(text: str, begin: str, end: str, before: str, after: str) -> str:
    a = text.find(begin)
    if a == -1:
        raise SystemExit(f"PathMask: missing section {begin}")
    b = text.find(end, a + len(begin))
    if b == -1:
        raise SystemExit(f"PathMask: missing end section {end}")
    return text[:a] + one(text[a:b], before, after, begin) + text[b:]


namei = ROOT / "fs/namei.c"
text = namei.read_text()
text = one(
    text, "#include <linux/uaccess.h>",
    "#include <linux/uaccess.h>\n#include <linux/pathmask.h>", "namei include")
start = text.index("int inode_permission(struct user_namespace")
stop = text.index("EXPORT_SYMBOL(inode_permission);", start)
portion = text[start:stop]
portion = one(
    portion, "\tint retval;\n",
    "\tint retval;\n\n\tif (unlikely(pathmask_should_hide_inode(inode)))\n"
    "\t\treturn -ENOENT;\n", "inode_permission")
text = text[:start] + portion + text[stop:]
namei.write_text(text)

stat = ROOT / "fs/stat.c"
text = stat.read_text()
text = one(
    text, "#include <linux/uaccess.h>",
    "#include <linux/uaccess.h>\n#include <linux/pathmask.h>", "stat include")
start = text.index("int vfs_getattr(")
stop = text.index("EXPORT_SYMBOL(vfs_getattr);", start)
portion = text[start:stop]
portion = one(
    portion, "\tretval = security_inode_getattr(path);",
    "\tif (unlikely(pathmask_should_hide_inode(d_inode(path->dentry))))\n"
    "\t\treturn -ENOENT;\n\n"
    "\tretval = security_inode_getattr(path);", "vfs_getattr")
text = text[:start] + portion + text[stop:]
stat.write_text(text)

readdir = ROOT / "fs/readdir.c"
text = readdir.read_text()
text = one(
    text, "#include <linux/uaccess.h>",
    "#include <linux/uaccess.h>\n#include <linux/pathmask.h>", "readdir include")
for before, after, label in (
    ("struct getdents_callback {\n\tstruct dir_context ctx;",
     "struct getdents_callback {\n\tstruct dir_context ctx;\n\tdev_t pm_dev;",
     "getdents callback"),
    ("struct getdents_callback64 {\n\tstruct dir_context ctx;",
     "struct getdents_callback64 {\n\tstruct dir_context ctx;\n\tdev_t pm_dev;",
     "getdents64 callback"),
    ("struct compat_getdents_callback {\n\tstruct dir_context ctx;",
     "struct compat_getdents_callback {\n\tstruct dir_context ctx;\n\tdev_t pm_dev;",
     "compat callback"),
):
    text = one(text, before, after, label)

for start, stop in (
    ("static bool filldir(", "SYSCALL_DEFINE3(getdents,"),
    ("static bool filldir64(", "SYSCALL_DEFINE3(getdents64,"),
    ("static bool compat_filldir(", "COMPAT_SYSCALL_DEFINE3(getdents,"),
):
    text = scoped(
        text, start, stop,
        "\tbuf->error = -EINVAL;",
        "\tif (unlikely(pathmask_should_hide_dirent(buf->pm_dev, ino)))\n"
        "\t\treturn true;\n"
        "\tbuf->error = -EINVAL;")

for start, stop in (
    ("SYSCALL_DEFINE3(getdents,", "struct getdents_callback64"),
    ("SYSCALL_DEFINE3(getdents64,", "#ifdef CONFIG_COMPAT"),
    ("COMPAT_SYSCALL_DEFINE3(getdents,", "#endif"),
):
    text = scoped(
        text, start, stop,
        "\tif (!f.file)\n\t\treturn -EBADF;\n\n\terror = iterate_dir",
        "\tif (!f.file)\n\t\treturn -EBADF;\n\n"
        "\tbuf.pm_dev = file_inode(f.file)->i_sb->s_dev;\n"
        "\terror = iterate_dir")
readdir.write_text(text)
print("PathMask: injected inode_permission, vfs_getattr and 3 getdents variants")
