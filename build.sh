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
TARGET_SYSTEM=$4

echo "TARGET_DEVICE: $TARGET_DEVICE"

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

echo "Clone AnyKernel3 for packing kernel (repo: https://github.com/Sayemx18/AKalioth)"
git clone https://github.com/Sayemx18/AKalioth -b main --single-branch --depth=1 anykernel

# Add date to local version
local_version_str="-perf"
local_version_date_str="-$(date +%Y%m%d)-${GIT_COMMIT_ID}-perf"

sed -i "s/${local_version_str}/${local_version_date_str}/g" arch/arm64/configs/vendor/${TARGET_DEVICE}_defconfig


Build_AOSP(){
# ------------- Building for AOSP -------------
    echo "Building for AOSP......"
    make $MAKE_ARGS vendor/${TARGET_DEVICE}_defconfig

    SET_CONFIG
    
    make $MAKE_ARGS -j$(nproc)

    Image_Repack

    echo "Build for AOSP finished."

    # ------------- End of Building for AOSP -------------
}


Build_MIUI(){
    # ------------- Building for MIUI -------------


    echo "Clearning [out/] and build for MIUI....."
    rm -rf out/

    dts_source=arch/arm64/boot/dts/vendor/qcom

    # Backup dts
    cp -a ${dts_source} .dts.bak

    # Correct panel dimensions on MIUI builds
    sed -i 's/<154>/<1537>/g' ${dts_source}/dsi-panel-j1s*
    sed -i 's/<154>/<1537>/g' ${dts_source}/dsi-panel-j2*
    sed -i 's/<155>/<1544>/g' ${dts_source}/dsi-panel-j3s-37-02-0a-dsc-video.dtsi
    sed -i 's/<155>/<1545>/g' ${dts_source}/dsi-panel-j11-38-08-0a-fhd-cmd.dtsi
    sed -i 's/<155>/<1546>/g' ${dts_source}/dsi-panel-k11a-38-08-0a-dsc-cmd.dtsi
    sed -i 's/<155>/<1546>/g' ${dts_source}/dsi-panel-l11r-38-08-0a-dsc-cmd.dtsi
    sed -i 's/<70>/<695>/g' ${dts_source}/dsi-panel-j11-38-08-0a-fhd-cmd.dtsi
    sed -i 's/<70>/<695>/g' ${dts_source}/dsi-panel-j3s-37-02-0a-dsc-video.dtsi
    sed -i 's/<70>/<695>/g' ${dts_source}/dsi-panel-k11a-38-08-0a-dsc-cmd.dtsi
    sed -i 's/<70>/<695>/g' ${dts_source}/dsi-panel-l11r-38-08-0a-dsc-cmd.dtsi
    sed -i 's/<71>/<710>/g' ${dts_source}/dsi-panel-j1s*
    sed -i 's/<71>/<710>/g' ${dts_source}/dsi-panel-j2*

    # Enable back mi smartfps while disabling qsync min refresh-rate
    sed -i 's/\/\/ mi,mdss-dsi-pan-enable-smart-fps/mi,mdss-dsi-pan-enable-smart-fps/g' ${dts_source}/dsi-panel*
    sed -i 's/\/\/ mi,mdss-dsi-smart-fps-max_framerate/mi,mdss-dsi-smart-fps-max_framerate/g' ${dts_source}/dsi-panel*
    sed -i 's/\/\/ qcom,mdss-dsi-pan-enable-smart-fps/qcom,mdss-dsi-pan-enable-smart-fps/g' ${dts_source}/dsi-panel*
    sed -i 's/qcom,mdss-dsi-qsync-min-refresh-rate/\/\/qcom,mdss-dsi-qsync-min-refresh-rate/g' ${dts_source}/dsi-panel*

    # Enable back refresh rates supported on MIUI
    sed -i 's/120 90 60/120 90 60 50 30/g' ${dts_source}/dsi-panel-g7a-36-02-0c-dsc-video.dtsi
    sed -i 's/120 90 60/120 90 60 50 30/g' ${dts_source}/dsi-panel-g7a-37-02-0a-dsc-video.dtsi
    sed -i 's/120 90 60/120 90 60 50 30/g' ${dts_source}/dsi-panel-g7a-37-02-0b-dsc-video.dtsi
    sed -i 's/144 120 90 60/144 120 90 60 50 48 30/g' ${dts_source}/dsi-panel-j3s-37-02-0a-dsc-video.dtsi


    # Enable back brightness control from dtsi
    sed -i 's/\/\/39 00 00 00 00 00 03 51 03 FF/39 00 00 00 00 00 03 51 03 FF/g' ${dts_source}/dsi-panel-j9-38-0a-0a-fhd-video.dtsi
    sed -i 's/\/\/39 00 00 00 00 00 03 51 0D FF/39 00 00 00 00 00 03 51 0D FF/g' ${dts_source}/dsi-panel-j2-p2-1-38-0c-0a-dsc-cmd.dtsi
    sed -i 's/\/\/39 00 00 00 00 00 05 51 0F 8F 00 00/39 00 00 00 00 00 05 51 0F 8F 00 00/g' ${dts_source}/dsi-panel-j1s-42-02-0a-dsc-cmd.dtsi
    sed -i 's/\/\/39 00 00 00 00 00 05 51 0F 8F 00 00/39 00 00 00 00 00 05 51 0F 8F 00 00/g' ${dts_source}/dsi-panel-j1s-42-02-0a-mp-dsc-cmd.dtsi
    sed -i 's/\/\/39 00 00 00 00 00 05 51 0F 8F 00 00/39 00 00 00 00 00 05 51 0F 8F 00 00/g' ${dts_source}/dsi-panel-j2-mp-42-02-0b-dsc-cmd.dtsi
    sed -i 's/\/\/39 00 00 00 00 00 05 51 0F 8F 00 00/39 00 00 00 00 00 05 51 0F 8F 00 00/g' ${dts_source}/dsi-panel-j2-p2-1-42-02-0b-dsc-cmd.dtsi
    sed -i 's/\/\/39 00 00 00 00 00 05 51 0F 8F 00 00/39 00 00 00 00 00 05 51 0F 8F 00 00/g' ${dts_source}/dsi-panel-j2s-mp-42-02-0a-dsc-cmd.dtsi
    sed -i 's/\/\/39 01 00 00 00 00 03 51 00 00/39 01 00 00 00 00 03 51 00 00/g' ${dts_source}/dsi-panel-j2-38-0c-0a-dsc-cmd.dtsi
    sed -i 's/\/\/39 01 00 00 00 00 03 51 00 00/39 01 00 00 00 00 03 51 00 00/g' ${dts_source}/dsi-panel-j2-38-0c-0a-dsc-cmd.dtsi
    sed -i 's/\/\/39 01 00 00 00 00 03 51 03 FF/39 01 00 00 00 00 03 51 03 FF/g' ${dts_source}/dsi-panel-j11-38-08-0a-fhd-cmd.dtsi
    sed -i 's/\/\/39 01 00 00 00 00 03 51 03 FF/39 01 00 00 00 00 03 51 03 FF/g' ${dts_source}/dsi-panel-j9-38-0a-0a-fhd-video.dtsi
    sed -i 's/\/\/39 01 00 00 00 00 03 51 07 FF/39 01 00 00 00 00 03 51 07 FF/g' ${dts_source}/dsi-panel-j1u-42-02-0b-dsc-cmd.dtsi
    sed -i 's/\/\/39 01 00 00 00 00 03 51 07 FF/39 01 00 00 00 00 03 51 07 FF/g' ${dts_source}/dsi-panel-j2-42-02-0b-dsc-cmd.dtsi
    sed -i 's/\/\/39 01 00 00 00 00 03 51 07 FF/39 01 00 00 00 00 03 51 07 FF/g' ${dts_source}/dsi-panel-j2-p1-42-02-0b-dsc-cmd.dtsi
    sed -i 's/\/\/39 01 00 00 00 00 03 51 0F FF/39 01 00 00 00 00 03 51 0F FF/g' ${dts_source}/dsi-panel-j1u-42-02-0b-dsc-cmd.dtsi
    sed -i 's/\/\/39 01 00 00 00 00 03 51 0F FF/39 01 00 00 00 00 03 51 0F FF/g' ${dts_source}/dsi-panel-j2-42-02-0b-dsc-cmd.dtsi
    sed -i 's/\/\/39 01 00 00 00 00 03 51 0F FF/39 01 00 00 00 00 03 51 0F FF/g' ${dts_source}/dsi-panel-j2-p1-42-02-0b-dsc-cmd.dtsi
    sed -i 's/\/\/39 01 00 00 00 00 05 51 07 FF 00 00/39 01 00 00 00 00 05 51 07 FF 00 00/g' ${dts_source}/dsi-panel-j1s-42-02-0a-dsc-cmd.dtsi
    sed -i 's/\/\/39 01 00 00 00 00 05 51 07 FF 00 00/39 01 00 00 00 00 05 51 07 FF 00 00/g' ${dts_source}/dsi-panel-j1s-42-02-0a-mp-dsc-cmd.dtsi
    sed -i 's/\/\/39 01 00 00 00 00 05 51 07 FF 00 00/39 01 00 00 00 00 05 51 07 FF 00 00/g' ${dts_source}/dsi-panel-j2-mp-42-02-0b-dsc-cmd.dtsi
    sed -i 's/\/\/39 01 00 00 00 00 05 51 07 FF 00 00/39 01 00 00 00 00 05 51 07 FF 00 00/g' ${dts_source}/dsi-panel-j2-p2-1-42-02-0b-dsc-cmd.dtsi
    sed -i 's/\/\/39 01 00 00 00 00 05 51 07 FF 00 00/39 01 00 00 00 00 05 51 07 FF 00 00/g' ${dts_source}/dsi-panel-j2s-mp-42-02-0a-dsc-cmd.dtsi
    sed -i 's/\/\/39 01 00 00 01 00 03 51 03 FF/39 01 00 00 01 00 03 51 03 FF/g' ${dts_source}/dsi-panel-j11-38-08-0a-fhd-cmd.dtsi
    sed -i 's/\/\/39 01 00 00 11 00 03 51 03 FF/39 01 00 00 11 00 03 51 03 FF/g' ${dts_source}/dsi-panel-j2-p2-1-38-0c-0a-dsc-cmd.dtsi

    make $MAKE_ARGS vendor/${TARGET_DEVICE}_defconfig

    SET_CONFIG MIUI

    make $MAKE_ARGS -j$(nproc)

    Image_Repack MIUI
    # ------------- End of Building for MIUI -------------

}

SET_CONFIG(){
    if [ "$1" == "MIUI" ]; then
        scripts/config --file out/.config \
            --set-str STATIC_USERMODEHELPER_PATH /system/bin/micd \
            -e PERF_CRITICAL_RT_TASK	\
            -e SF_BINDER		\
            -e OVERLAY_FS		\
            -d DEBUG_FS \
            -e MIGT \
            -e MIGT_ENERGY_MODEL \
            -e MIHW \
            -e PACKAGE_RUNTIME_INFO \
            -e BINDER_OPT \
            -e KPERFEVENTS \
            -e MILLET \
            -e PERF_HUMANTASK \
            -d LTO_CLANG \
            -d LOCALVERSION_AUTO \
            -e SF_BINDER \
            -e XIAOMI_MIUI \
            -d MI_MEMORY_SYSFS \
            -e TASK_DELAY_ACCT \
            -e MIUI_ZRAM_MEMORY_TRACKING \
            -d CONFIG_MODULE_SIG_SHA512 \
            -d CONFIG_MODULE_SIG_HASH \
            -e MI_FRAGMENTION \
            -e PERF_HELPER \
            -e BOOTUP_RECLAIM \
            -e MI_RECLAIM \
            -e RTMM
    fi
    
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
        echo "The file [out/arch/arm64/boot/Image.gz] exists. AOSP Build successfully."
    else
        echo "The file [out/arch/arm64/boot/Image.gz] does not exist. Seems AOSP build failed."
        exit 1
    fi

      # KPM Patch - only for sukisu-ultra with KPM enabled
    if [[ "$KPM_ENABLE" -eq 1 && "$KSU_VERSION" == "sukisu-ultra" ]]; then
        echo "Applying KPM patch..."
        if Patch_KPM; then
            echo "KPM patch applied successfully"
        else
            echo "Warning: KPM patch failed, continuing with original Image"
            echo "This may cause boot issues if KPM features are required"
            # You can choose to exit here if KPM is critical:
            # exit 1
        fi
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

    # Restore modified dts
    if [ "$1" == "MIUI" ]; then
        rm -rf ${dts_source}
        mv .dts.bak ${dts_source}
    fi

    cp out/arch/arm64/boot/Image.gz anykernel/
    cp out/arch/arm64/boot/dtb anykernel/

    if [ -f "out/arch/arm64/boot/dtbo.img" ]; then
        cp out/arch/arm64/boot/dtbo.img anykernel/
    fi

    cd anykernel 

    if [ "$1" == "MIUI" ]; then
        ZIP_FILENAME=N0Kernel_MIUI_${TARGET_DEVICE}_${KSU_ZIP_STR}_$(date +'%Y%m%d_%H%M%S')_anykernel3_${GIT_COMMIT_ID}.zip
    else
        ZIP_FILENAME=N0Kernel_16.3.9_${TARGET_DEVICE}_${KSU_ZIP_STR}_$(date +'%Y%m%d_%H%M%S')_anykernel3_${GIT_COMMIT_ID}.zip
    fi

    zip -r9 $ZIP_FILENAME ./* -x .git .gitignore out/ ./*.zip

    mv $ZIP_FILENAME ../

    cd ..
}

Patch_KPM(){
    echo "Starting KPM patching process..."
    
    # Store original directory
    ORIGINAL_DIR=$(pwd)
    
    # Create a temporary directory for patching
    PATCH_DIR="$KERNEL_SRC/kpm_patch_temp"
    mkdir -p "$PATCH_DIR"
    cd "$PATCH_DIR"
    
    # Download the patch script
    echo "Downloading KMP patch script..."
    if ! curl -LSs "https://raw.githubusercontent.com/ShirkNeko/SukiSU_patch/refs/heads/main/kmp/patch_linux" -o patch_linux; then
        echo "Error: Failed to download patch script"
        cd "$ORIGINAL_DIR"
        return 1
    fi
    
    # Make it executable
    chmod +x patch_linux
    
    # Copy the Image.gz to patch directory
    if [ ! -f "$KERNEL_SRC/out/arch/arm64/boot/Image.gz" ]; then
        echo "Error: Image.gz not found at expected location"
        cd "$ORIGINAL_DIR"
        return 1
    fi
    
    # Extract Image.gz for patching
    echo "Extracting Image.gz for patching..."
    cp "$KERNEL_SRC/out/arch/arm64/boot/Image.gz" ./
    gunzip Image.gz
    
    if [ ! -f "Image" ]; then
        echo "Error: Failed to extract Image from Image.gz"
        cd "$ORIGINAL_DIR"
        return 1
    fi
    
    # Run the patch
    echo "Applying KMP patch..."
    if ./patch_linux; then
        echo "KMP patch applied successfully"
        
        # Check if patched image exists
        if [ -f "oImage" ]; then
            # Replace original with patched version
            mv oImage Image
            
            # Compress back to Image.gz
            echo "Compressing patched Image..."
            gzip Image
            
            # Replace the original Image.gz
            cp Image.gz "$KERNEL_SRC/out/arch/arm64/boot/Image.gz"
            echo "KMP patched Image.gz has been updated"
            
            # Cleanup
            cd "$ORIGINAL_DIR"
            rm -rf "$PATCH_DIR"
            return 0
        else
            echo "Error: Patched image (oImage) not found after patching"
            cd "$ORIGINAL_DIR"
            rm -rf "$PATCH_DIR"
            return 1
        fi
    else
        echo "Error: KMP patch failed"
        cd "$ORIGINAL_DIR"
        rm -rf "$PATCH_DIR"
        return 1
    fi
}

if [ "$TARGET_SYSTEM" == "aosp" ];then
    Build_AOSP
elif [ "$TARGET_SYSTEM" == "miui" ];then
    Build_MIUI
else
    Build_AOSP
    Build_MIUI
fi
    
echo "Done. The flashable zip is: [./$ZIP_FILENAME]"
