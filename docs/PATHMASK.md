# PathMask integration — SULTAN17 (17.0.0-cp3a)

PathMask is built as a separate, matching loadable kernel module. It is NOT embedded in Image.lz4, silently enabled on boot, or applied to another branch.

- Upstream: https://github.com/Andrea-lyz/LKM-PathMask
- Pinned source commit: 4adf552ab653fcd6c4512fc8927d8f172fa9536b (v2.8.3)
- KMI: android14-6.1
- Kernel: this exact variant's patched Sultan kernel, with its Module.symvers and GCC cross-toolchain.
- Dependencies: pinned by the upstream kernel/deps.lst and .sdk-version.

## CI artifacts

On 17.0.0-cp3a only, the shared build.yml builds PathMask after building the kernel. Its two separate artifacts are:

1. Sultan_* — the original AnyKernel files (unchanged).
2. PathMask_* — a matching pathmask.ko, optional companion procguard.ko, and a KernelSU/KernelSU Next installation ZIP.

The PathMask Integration Test workflow builds zuma + ksu-next-susfs-vpnhide on a cp3a push and can also be dispatched manually. The reusable build.yml also builds the matching module for other combinations only if the branch input is 17.0.0-cp3a.

Install the Sultan kernel image first. Then install the **matching PathMask _ksu.zip** via KernelSU Manager, NOT by flashing it in recovery. Do not reuse a .ko with another Sultan variant/release even if both are android14-6.1. The ZIP deliberately disables the upstream generic-KMI updateJson channel, to avoid silently replacing this device-specific module.

PathMask can hook filesystem paths used by SUSFS/NoMount/VPNHide. Compilation does not prove runtime compatibility or absence of hook conflicts. The installer remains optional and separate from AnyKernel.

## Manual builds

After patching and building the Sultan kernel normally:

    bash ./gscripts/build_pathmask.sh zuma ksu-next-susfs-vpnhide

This requires CONFIG_MODULES=y, out/Module.symvers, git, make, zip, and the cross-compiler already used for the kernel. It aborts if prerequisites are missing.

## Device verification

    adb shell su -c 'grep -E "^(pathmask|procguard) " /proc/modules'
    adb shell su -c 'dmesg | grep -i -e pathmask -e module_layout | tail -50'

Confirm kernel-module loading, logs, and ordinary system behavior before enabling new hiding rules. No hardware/runtime test is claimed.
