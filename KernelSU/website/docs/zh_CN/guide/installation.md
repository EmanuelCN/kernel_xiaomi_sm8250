# 安装 {#title}

## 检查您的设备是否被支持 {#check-if-supported}

从 [GitHub Releases](https://github.com/tiann/KernelSU/releases) 或 [酷安](https://www.coolapk.com/apk/me.weishu.kernelsu) 下载 KernelSU 管理器应用，然后将应用程序安装到设备并打开：

- 如果应用程序显示 “不支持”，则表示您的设备不支持 KernelSU，你需要自己编译设备的内核才能使用，KernelSU 官方不会也永远不会为你提供一个可以刷写的 boot 镜像。
- 如果应用程序显示 “未安装”，那么 KernelSU 支持您的设备；可以进行下一步操作。

:::info
对于显示“不支持”的设备，这里有一个[非官方支持设备列表](unofficially-support-devices.md)，你可以用这个列表里面的内核自行编译。
:::

## 备份你的 boot.img {#backup-boot-image}

在进行刷机操作之前，你必须先备份好自己的原厂 boot.img。如果你后续刷机出现了任何问题，你都可以通过使用 fastboot 刷回原厂 boot 来恢复系统。

::: warning
任何刷机操作都是有风险的，请务必做好这一步再进行下一步操作！！必要时你还可以备份你手机的所有数据。
:::

## 必备知识 {#acknowage}

### ADB 和 fastboot {#adb-and-fastboot}

此教程默认你会使用 ADB 和 fastboot 工具，如果你没有了解过，建议使用搜索引擎先学习相关知识。

### KMI

KMI 全称 Kernel Module Interface，相同 KMI 的内核版本是**兼容的** 这也是 GKI 中“通用”的含义所在；反之，如果 KMI 不同，那么这些内核之间无法互相兼容，刷入与你设备 KMI 不同的内核镜像可能会导致死机。

具体来说，对 GKI 的设备，其内核版本格式应该如下：

```txt
KernelRelease :=
Version.PatchLevel.SubLevel-AndroidRelease-KmiGeneration-suffix
w      .x         .y       -zzz           -k            -something
```

其中，`w.x-zzz-k` 为 KMI 版本。例如，一个设备内核版本为`5.10.101-android12-9-g30979850fc20`，那么它的 KMI 为 `5.10-android12-9`；理论上刷入其他这个 KMI 的内核也能正常开机。

::: tip
请注意，内核版本中的 SubLevel 不属于 KMI 的范畴！也就是说 `5.10.101-android12-9-g30979850fc20` 与 `5.10.137-android12-9-g30979850fc20` 的 KMI 相同！
:::

### 内核版本与 Android 版本 {#kernel-version-vs-android-version}

请注意：**内核版本与 Android 版本并不一定相同！**

如果您发现您的内核版本是 `android12-5.10.101`，然而你 Android 系统的版本为 Android 13 或者其他；请不要觉得奇怪，因为 Android 系统的版本与 Linux 内核的版本号不一定是一致的；Linux 内核的版本号一般与**设备出厂的时候自带的 Android 系统的版本一致**，如果后续 Android 系统升级，内核版本一般不会发生变化。如果你需要刷机，**请以内核版本为准！！**

## 安装介绍 {#installation-introduction}

KernelSU 的安装方法有如下几种，各自适用于不同的场景，请按需选择：

1. 使用自定义 Recovery（如 TWRP）安装
2. 使用内核刷写 App，如 （Franco Kernel Manager）安装
3. 使用 KernelSU 提供的 boot.img 使用 fastboot 安装
4. 手动修补 boot.img 然后安装

## 使用自定义 Recovery 安装 {#install-by-recovery}

前提：你的设备必须有自定义的 Recovery，如 TWRP；如果没有或者只有官方 Recovery，请使用其他方法。

步骤：

1. 在 KernelSU 的 [Release 页面](https://github.com/tiann/KernelSU/releases) 下载与你手机版本匹配的以 AnyKernel3 开头的 zip 刷机包；例如，手机内核版本为 `android12-5.10.66`，那么你应该下载 `AnyKernel3-android12-5.10.66_yyyy-MM.zip` 这个文件（其中 `yyyy` 为年份，`MM` 为月份）。
2. 重启手机进入 TWRP。
3. 使用 adb 将 AnyKernel3-*.zip 放到手机 /sdcard 然后在 TWRP 图形界面选择安装；或者你也可以直接 `adb sideload AnyKernel-*.zip` 安装。

PS. 这种方法适用于任何情况下的安装（不限于初次安装或者后续升级），只要你用 TWRP 就可以操作。

## 使用内核刷写 App 安装 {#install-by-kernel-flasher}

前提：你的设备必须已经 root。例如你已经安装了 Magisk 获取了 root，或者你已经安装了旧版本的 KernelSU 需要升级到其他版本的 KernelSU；如果你的设备无 root，请尝试其他方法。

步骤：

1. 下载 AnyKernel3 的刷机包；下载方法参考 *使用自定义 Recovery 安装*那一节的内容。
2. 打开内核刷写 App 使用提供的 AnyKernel3 刷机包刷入。

如果你之前没有用过内核刷写 App，那么下面几个是比较流行的：

1. [Kernel Flasher](https://github.com/capntrips/KernelFlasher/releases)
2. [Franco Kernel Manager](https://play.google.com/store/apps/details?id=com.franco.kernel)
3. [Ex Kernel Manager](https://play.google.com/store/apps/details?id=flar2.exkernelmanager)

PS. 这种方法在升级 KernelSU 的时候较为方便，无需电脑即可完成（注意备份！）。

## 使用 KernelSU 提供的 boot.img 安装 {#install-by-kernelsu-boot-image}

这种方法无需你有 TWRP，也不需要你的手机有 root 权限；适用于你初次安装 KernelSU。

### 找到合适的 boot.img {#found-propery-image}

KernelSU 为 GKI 设备提供了通用的 boot.img，您应该将 boot.img 刷写到设备的 boot 分区。

您可以从 [GitHub Release](https://github.com/tiann/KernelSU/releases) 下载 boot.img, 请注意您应该使用正确版本的 boot.img. 例如，如果您的设备显示内核是 `android12-5.10.101`, 需要下载 `android-5.10.101_yyyy-MM.boot-<format>.img`.

其中 `<format>` 指的是你的官方 boot.img 的内核压缩格式，请检查您原有 boot.img 的内核压缩格式，您应该使用正确的格式，例如 `lz4`、`gz`；如果是用不正确的压缩格式，刷入 boot 后可能无法开机。

::: info
1. 您可以通过 magiskboot 来获取你原来 boot 的压缩格式；当然您也可以询问与您机型相同的其他更有经验的童鞋。另外，内核的压缩格式通常不会发生变化，如果您使用某个压缩格式成功开机，后续可优先尝试这个格式。
2. 小米设备通常使用 `gz` 或者 **不压缩**。
3. Pixel 设备有些特殊，请查看下面的教程。
:::

### 将 boot.img 刷入设备 {#flash-boot-image}

使用 `adb` 连接您的设备，然后执行 `adb reboot bootloader` 进入 fastboot 模式，然后使用此命令刷入 KernelSU：

```sh
fastboot flash boot boot.img
```

::: info
如果你的设备支持 `fastboot boot`，可以先使用 `fastboot boot boot.img` 来先尝试使用 boot.img 引导系统，如果出现意外，再重启一次即可开机。
:::

### 重启 {#reboot}

刷入完成后，您应该重新启动您的设备：

```sh
fastboot reboot
```

## 手动修补 boot.img {#patch-boot-image}

对于某些设备来说，其 boot.img 格式不那么常见，比如不是 `lz4`, `gz` 和未压缩；最典型的就是 Pixel，它 boot.img 的格式是 `lz4_legacy` 压缩，ramdisk 可能是 `gz` 也可能是 `lz4_legacy` 压缩；此时如果你直接刷入 KernelSU 提供的 boot.img，手机可能无法开机；这时候，你可以通过手动修补 boot.img 来实现。

修补方法总体有两种：

1. [Android-Image-Kitchen](https://forum.xda-developers.com/t/tool-android-image-kitchen-unpack-repack-kernel-ramdisk-win-android-linux-mac.2073775/)
2. [magiskboot](https://github.com/topjohnwu/Magisk/releases)

其中，Android-Image-Kitchen 适用于 PC 上操作，magiskboot 需要手机配合。

### 准备 {#patch-preparation}

1. 获取你手机的原厂 boot.img；你可以通过你手机的线刷包解压后之间获取，如果你是卡刷包，那你也许需要[payload-dumper-go](https://github.com/ssut/payload-dumper-go)
2. 下载 KernelSU 提供的与你设备 KMI 版本一致的 AnyKernel3 刷机包（可以参考 *自定义 TWRP 刷入一节*)。
3. 解压缩 AnyKernel3 刷机包，获取其中的 `Image` 文件，此文件为 KernelSU 的内核文件。

### 使用 Android-Image-Kitchen {#using-android-image-kitchen}

1. 下载 Android-Image-Kitchen 至你电脑
2. 将手机原厂 boot.img 放入 Android-Image-Kitchen 根目录
3. 在 Android-Image-Kitchen 根目录执行 `./unpackimg.sh boot.img`；此命名会将 boot.img 拆开，你会得到若干文件。
4. 将`split_img` 目录中的 `boot.img-kernel` 替换为你从 AnyKernel3 解压出来的 `Image`（注意名字改为 boot.img-kernel)。
5. 在 Android-Image-Kitchecn 根目录执行 `./repackimg.sh`；此时你会得到一个 `image-new.img` 的文件；使用此 boot.img 通过 fastboot 刷入即可（刷入方法参考上一节）。 

### 使用 magiskboot {#using magiskboot}

1. 在 Magisk 的 [Release 页面](https://github.com/topjohnwu/Magisk/releases) 下载最新的 Magisk 安装包。
2. 将 Magisk-*.apk 重命名为 Magisk-vesion.zip 然后解压缩。
3. 将解压后的 `Magisk-v25.2/lib/arm64-v8a/libmagiskboot.so` 文件，使用 adb push 到手机：`adb push Magisk-v25.2/lib/arm64-v8a/libmagiskboot.so /data/local/tmp/magiskboot`
4. 使用 adb 将原厂 boot.img 和 AnyKernel3 中的 Image 推送到手机
5. adb shell 进入 /data/local/tmp/ 目录，然后赋予刚 push 文件的可执行权限 `chmod +x magiskboot`
6. adb shell 进入 /data/local/tmp/ 目录，执行 `./magiskboot unpack boot.img` 此时会解包 `boot.img` 得到一个叫做 `kernel` 的文件，这个文件为你原厂的 kernel
7. 使用 `Image` 替换 `kernel`: `mv -f Image kernel`
8. 执行 `./magiskboot repack boot.img` 打包 img，此时你会得到一个 `new-boot.img` 的文件，使用这个文件 fastboot 刷入设备即可。

## 其他变通方法 {#other-methods}

其实所有这些安装方法的主旨只有一个，那就是**替换原厂的内核为 KernelSU 提供的内核**；只要能实现这个目的，就可以安装；比如以下是其他可行的方法：

1. 首先安装 Magisk，通过 Magisk 获取 root 权限后使用内核刷写器刷入 KernelSU 的 AnyKernel 包。
2. 使用某些 PC 上的刷机工具箱刷入 KernelSU 提供的内核。
