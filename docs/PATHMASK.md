# Native built-in PathMask — Sultan17 cp3a (zuma)

This branch implements PathMask-like scoped VFS masking **inside the kernel**
without external `.ko`, Kretprobes, or separate KernelSU module. Sultan has
`CONFIG_INTEGRATE_MODULES=y` and explicitly disallows `CONFIG_MODULES=y`.

## Kernel integration

- `CONFIG_PATHMASK=y` in `arch/arm64/configs/zuma_defconfig`
- Native VFS hooks: `inode_permission`, `vfs_getattr`,
  `getdents`, `getdents64`, and compat `getdents`
- Identity: device + inode; scope: deny UID list, allow UID list, global
- UIDs 0 (root) and 1000 (system) exempt in every mode
- No default targets. Starts disabled. Up to 64 paths and 256 UIDs per list
- Root-only runtime control: `/proc/pathmask_ctl` (CAP_SYS_ADMIN)

## Termux

```sh
su -c 'cat /proc/pathmask_ctl'
su -c 'echo "add /system_ext/app/SoterService" > /proc/pathmask_ctl'
su -c 'echo "deny_uid 12345" > /proc/pathmask_ctl'
su -c 'echo "mode deny" > /proc/pathmask_ctl'
su -c 'echo "enable 1" > /proc/pathmask_ctl'
```

Replace 12345 with the target app's **Linux UID**, not a package name. Check
with `su -c 'cmd package list packages -U'`.

Reset everything:

```sh
su -c 'echo "enable 0" > /proc/pathmask_ctl'
su -c 'echo clear > /proc/pathmask_ctl'
```

Commands also include `del PATH`, `remove_deny_uid UID`,
`allow_uid UID`, `remove_allow_uid UID`, `mode allow`, `mode global`,
and `dirents 0|1`.

Rules are **not persistent across reboot**; reapply via a root boot script
only after target filesystems are mounted. Never hide critical framework
paths: the code rejects paths under `/system/framework`, `/system/bin`,
`/system/lib`, `/system_ext/framework`, `/product/framework`,
`/apex`, `/vendor/bin`, `/vendor/lib`.

## Differences from upstream

This is a **native VFS reimplementation of PathMask semantics**, not a
verbatim port of upstream [LKM-PathMask](https://github.com/Andrea-lyz/LKM-PathMask).
It does not include the upstream WebUI, ProcGuard, dynamic `.ko`,
automatic package-to-UID resolution, isolated-UID parent mapping,
syscall write-op policy, or mountinfo filtering.

The former `gscripts/build_pathmask.sh` LKM workflow is superseded.
Only the flashable Sultan AnyKernel ZIP is produced. GitHub CI confirms
build and packaging, **not physical-device runtime safety**.
