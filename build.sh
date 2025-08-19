#!/bin/bash

# Some logics of this script are copied from [scripts/build_kernel]. Thanks to UtsavBalar1231.

# Ensure the script exits on error
set -e

TOOLCHAIN_PATH=$HOME/toolchain/clang-13289611/bin
GIT_COMMIT_ID=$(git rev-parse --short=8 HEAD)
TARGET_DEVICE=$1

# Fix: Explicitly set host tools compiler (x86_64)
export HOSTCC=gcc
export HOSTCFLAGS="-O2 -Wall"

if [ -z "$1" ]; then
    echo "Error: No argument provided, please specific a target device." 
    echo "If you need KernelSU, please add [ksu] as the second arg."
    echo "Examples:"
    echo "Build for lmi(K30 Pro/POCO F2 Pro) without KernelSU:"
    echo "    bash build.sh lmi"
    echo "Build for umi(Mi10) with KernelSU:"
    echo "    bash build.sh umi ksu"
    exit 1
fi

if [ ! -d $TOOLCHAIN_PATH ]; then
    echo "TOOLCHAIN_PATH [$TOOLCHAIN_PATH] does not exist."
    echo "Please ensure the toolchain is there, or change TOOLCHAIN_PATH in the script to your toolchain path."
    exit 1
fi

echo "TOOLCHAIN_PATH: [$TOOLCHAIN_PATH]"
export PATH="$TOOLCHAIN_PATH:$PATH"

if ! command -v ld.lld >/dev/null 2>&1; then
    echo "[ld.lld] does not exist, please check your LLVM toolchain."
    exit 1
fi

if ! command -v clang >/dev/null 2>&1; then
    echo "[clang] does not exist, please check your environment."
    exit 1
fi

# Enable ccache for speed up compiling 
export CCACHE_DIR="$HOME/.cache/ccache_mikernel" 
export CC="ccache gcc"
export CXX="ccache g++"
export PATH="/usr/lib/ccache:$PATH"
echo "CCACHE_DIR: [$CCACHE_DIR]"


# Fixed MAKE_ARGS - Added HOSTCC/HOSTCFLAGS and kept LLVM flags
MAKE_ARGS="
  ARCH=arm64 
  SUBARCH=arm64 
  O=out 
  LLVM=1 
  LLVM_IAS=1 
  CC=clang 
  HOSTCC=gcc
  HOSTCFLAGS=-O2 -Wall
  CROSS_COMPILE=aarch64-linux-gnu- 
  CROSS_COMPILE_ARM32=arm-linux-gnueabi- 
  CLANG_TRIPLE=aarch64-linux-gnu-
"


if [ "$1" == "j1" ]; then
    make $MAKE_ARGS -j1
    exit
fi

if [ "$1" == "continue" ]; then
    make $MAKE_ARGS -j$(nproc)
    exit
fi

if [ ! -f "arch/arm64/configs/vendor/${TARGET_DEVICE}_defconfig" ]; then
    echo "No target device [${TARGET_DEVICE}] found."
    echo "Avaliable defconfigs, please choose one target from below down:"
    ls arch/arm64/configs/*_defconfig
    exit 1
fi


# Check clang is existing.
echo "[clang --version]:"
clang --version

# Initialize variable
KERNEL_SRC=$(pwd)
SuSFS_ENABLE=0
KPM_ENABLE=0
KSU_VERSION=$2
ADDITIONAL=$3
KERNEL_VERSION=$4

echo "TARGET_DEVICE: $TARGET_DEVICE"
echo "KERNEL_VERSION: $KERNEL_VERSION"

KSU_ENABLE=$([[ "$KSU_VERSION" == "ksu" || "$KSU_VERSION" == "rksu" || "$KSU_VERSION" == "sukisu" || "$KSU_VERSION" == "sukisu-ultra" ]] && echo 1 || echo 0)

if [ "$ADDITIONAL" == "susfs-kpm" ]; then
    SuSFS_ENABLE=1
    KPM_ENABLE=1
    echo "Enable SuSFS and KPM"
elif [ "$ADDITIONAL" == "susfs" ]; then
    SuSFS_ENABLE=1
    echo "Enable SuSFS"
elif [ "$ADDITIONAL" == "kpm" ]; then
    KPM_ENABLE=1
    echo "Enable KPM"
else 
    echo "The additional function is not enabled"
fi

if [ "$KSU_VERSION" == "ksu" ]; then
    KSU_ZIP_STR=KernelSU
    echo "KSU is enabled"
    curl -LSs "https://raw.githubusercontent.com/tiann/KernelSU/main/kernel/setup.sh" | bash -s v0.9.5
elif [[ "$KSU_VERSION" == "ksu" && "$SuSFS_ENABLE" -eq 1 ]]; then
    echo "Official KernelSU not supported SuSFS"
    exit 1
elif [[ "$KSU_VERSION" == "rksu" && "$SuSFS_ENABLE" -eq 1 ]]; then
    KSU_ZIP_STR=RKSU_SuSFS
    echo "RKSU && SuSFS is enabled"
    curl -LSs "https://raw.githubusercontent.com/rsuntk/KernelSU/main/kernel/setup.sh" | bash -s susfs-v1.5.5
elif [ "$KSU_VERSION" == "rksu" ]; then
    KSU_ZIP_STR=RKSU
    echo "RKSU is enabled"
    curl -LSs "https://raw.githubusercontent.com/rsuntk/KernelSU/main/kernel/setup.sh" | bash -s main
elif [[ "$KSU_VERSION" == "sukisu" && "$SuSFS_ENABLE" -eq 1 ]]; then
    KSU_ZIP_STR=SukiSU_SuSFS
    echo "SukiSU && SuSFS is enabled"
    curl -LSs "https://raw.githubusercontent.com/ShirkNeko/KernelSU/main/kernel/setup.sh" | bash -s susfs-dev
elif [ "$KSU_VERSION" == "sukisu" ]; then
    KSU_ZIP_STR=SukiSU
    echo "SukiSU is enabled"
    curl -LSs "https://raw.githubusercontent.com/ShirkNeko/KernelSU/main/kernel/setup.sh" | bash -s dev
elif [[ "$KSU_VERSION" == "sukisu-ultra" && "$SuSFS_ENABLE" -eq 1 ]]; then
    KSU_ZIP_STR="SukiSU-Ultra"
    echo "SukiSU-Ultra && SuSFS is enabled"
    curl -LSs "https://raw.githubusercontent.com/SukiSU-Ultra/SukiSU-Ultra/main/kernel/setup.sh" | bash -s susfs-main
elif [ "$KSU_VERSION" == "sukisu-ultra" ]; then
    KSU_ZIP_STR=SukiSU-Ultra
    echo "SukiSU-Ultra is enabled"
    curl -LSs "https://raw.githubusercontent.com/SukiSU-Ultra/SukiSU-Ultra/main/kernel/setup.sh" | bash -s nongki
else
    KSU_ZIP_STR=NoKernelSU
    echo "KSU is disabled"
fi

echo "Cleaning..."

rm -rf out/
rm -rf anykernel/

# Clone device-specific AnyKernel3 repository
Clone_AnyKernel() {
    case "$TARGET_DEVICE" in
        "alioth")
            echo "Clone AnyKernel3 for alioth (repo: https://github.com/Sayemx18/AKalioth)"
            git clone https://github.com/Sayemx18/AKalioth -b main --single-branch --depth=1 anykernel
            ;;
        "apollo")
            echo "Clone AnyKernel3 for apollo (repo: https://github.com/Sayemx18/AKapollo)"
            git clone https://github.com/Sayemx18/AKapollo -b main --single-branch --depth=1 anykernel
            ;;
        "munch")
            echo "Clone AnyKernel3 for munch (repo: https://github.com/Sayemx18/AKmunch)"
            git clone https://github.com/Sayemx18/AKmunch -b main --single-branch --depth=1 anykernel
            ;;
        *)
            echo "Unknown device: $TARGET_DEVICE. Using default AnyKernel3 for alioth..."
            git clone https://github.com/Sayemx18/AKalioth -b main --single-branch --depth=1 anykernel
            ;;
    esac
}

# Call the function to clone appropriate AnyKernel3
Clone_AnyKernel

# Add date to local version
local_version_str="-perf"
local_version_date_str="-$(date +%Y%m%d)-${GIT_COMMIT_ID}-perf"

sed -i "s/${local_version_str}/${local_version_date_str}/g" arch/arm64/configs/vendor/${TARGET_DEVICE}_defconfig


Build_Kernel(){
# ------------- Building Kernel -------------
    echo "Building kernel......"
    make $MAKE_ARGS vendor/${TARGET_DEVICE}_defconfig

    SET_CONFIG
    
    make $MAKE_ARGS -j$(nproc)

    Image_Repack

    echo "Kernel build finished."

    # ------------- End of Building Kernel -------------
}




SET_CONFIG(){    
    if [ "$KSU_ENABLE" -eq 1 ]; then
        scripts/config --file out/.config -e KSU
    else
        scripts/config --file out/.config -d KSU
    fi

    # Enable the KSU_MANUAL_HOOK for sukisu-ultra
    if [ "$KSU_VERSION" == "sukisu-ultra" ];then
        scripts/config --file out/.config -e KSU_MANUAL_HOOK
    else
        scripts/config --file out/.config -e KSU_MANUAL_HOOK
    fi

    # Config KPM 
    if [ "$KPM_ENABLE" -eq 1 ]; then
        scripts/config --file out/.config \
            -e KPM \
            -e KALLSYMS \
            -e KALLSYMS_ALL
    else 
        scripts/config --file out/.config \
            -d KPM \
            -d KALLSYMS \
            -d KALLSYMS_ALL
    fi

    if [ "$SuSFS_ENABLE" -eq 1 ];then
        scripts/config --file out/.config \
            -e KSU_SUSFS \
            -e KSU_SUSFS_HAS_MAGIC_MOUNT \
            -e KSU_SUSFS_SUS_PATH \
            -e KSU_SUSFS_SUS_MOUNT \
            -e KSU_SUSFS_AUTO_ADD_SUS_KSU_DEFAULT_MOUNT \
            -e KSU_SUSFS_AUTO_ADD_SUS_BIND_MOUNT \
            -e KSU_SUSFS_SUS_KSTAT \
            -e KSU_SUSFS_TRY_UMOUNT \
            -e KSU_SUSFS_AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT \
            -e KSU_SUSFS_SPOOF_UNAME \
            -e KSU_SUSFS_ENABLE_LOG \
            -e KSU_SUSFS_HIDE_KSU_SUSFS_SYMBOLS \
            -e KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG \
            -e KSU_SUSFS_OPEN_REDIRECT
     else
        scripts/config --file out/.config \
            -d KSU_SUSFS \
            -d KSU_SUSFS_HAS_MAGIC_MOUNT \
            -d KSU_SUSFS_SUS_PATH \
            -d KSU_SUSFS_SUS_MOUNT \
            -d KSU_SUSFS_AUTO_ADD_SUS_KSU_DEFAULT_MOUNT \
            -d KSU_SUSFS_AUTO_ADD_SUS_BIND_MOUNT \
            -d KSU_SUSFS_SUS_KSTAT \
            -d KSU_SUSFS_TRY_UMOUNT \
            -d KSU_SUSFS_AUTO_ADD_TRY_UMOUNT_FOR_BIND_MOUNT \
            -d KSU_SUSFS_SPOOF_UNAME \
            -d KSU_SUSFS_ENABLE_LOG \
            -d KSU_SUSFS_HIDE_KSU_SUSFS_SYMBOLS \
            -d KSU_SUSFS_SPOOF_CMDLINE_OR_BOOTCONFIG \
            -d KSU_SUSFS_OPEN_REDIRECT
    fi
}

Image_Repack(){
    if [ -f "out/arch/arm64/boot/Image.gz" ]; then
        echo "The file [out/arch/arm64/boot/Image.gz] exists. Build successful."
    else
        echo "The file [out/arch/arm64/boot/Image.gz] does not exist. Build failed."
        exit 1
    fi

    # KPM Patch
    if [[ "$KPM_ENABLE" -eq 1 && "$KSU_VERSION" == "sukisu-ultra" ]]; then
        Patch_KPM
    fi

    echo "Generating [out/arch/arm64/boot/dtb]......"
    find out/arch/arm64/boot/dts -name '*.dtb' -exec cat {} + >out/arch/arm64/boot/dtb

Generate_dtbo() {
    echo "Generating dtbo.img..."
    # Find all .dtbo files and combine them into dtbo.img
    find out/arch/arm64/boot/dts -name '*.dtbo' | sort | while read file; do
        cat "$file" >> out/arch/arm64/boot/dtbo.img
    done

    if [ -f "out/arch/arm64/boot/dtbo.img" ]; then
        echo "dtbo.img generated successfully."
    else
        echo "Warning: No .dtbo files found. dtbo.img not generated."
    fi
}

    cp out/arch/arm64/boot/Image.gz anykernel/
    cp out/arch/arm64/boot/dtb anykernel/

    if [ -f "out/arch/arm64/boot/dtbo.img" ]; then
        cp out/arch/arm64/boot/dtbo.img anykernel/
    fi

    cd anykernel 

    ZIP_FILENAME=N0Kernel_${KERNEL_VERSION}_${TARGET_DEVICE}_${KSU_ZIP_STR}_$(date +'%Y%m%d_%H%M%S')_anykernel3_${GIT_COMMIT_ID}.zip

    zip -r9 $ZIP_FILENAME ./* -x .git .gitignore out/ ./*.zip

    mv $ZIP_FILENAME ../

    cd ..
}

Patch_KPM(){
    cd out/arch/arm64/boot
    curl -LSs "https://raw.githubusercontent.com/ShirkNeko/SukiSU_patch/refs/heads/main/kmp/patch_linux" -o patch
    chmod +x patch
    ./patch
    if [ $? -eq 0 ]; then
        rm -f Image
        mv oImage Image
        echo "Image file repair complete"
    else
        echo "KPM Patch Failed, Use Original Image"
    fi
    
    cd $KERNEL_SRC

}

Build_Kernel

echo "Done. The flashable zip is: [./$ZIP_FILENAME]"
