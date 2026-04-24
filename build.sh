#!/bin/bash

abort()
{
    cd -
    echo "-----------------------------------------------"
    echo "Kernel compilation failed! Exiting..."
    echo "-----------------------------------------------"
    exit -1
}

unset_flags()
{
    cat << EOF
Usage: $(basename "$0") [options]
Options:
    -m, --model [value]     Specify the model code of the phone
    -k, --ksu [Y/n]         Include KernelSU
    -r, --recovery [y/N]    Compile kernel for an Android Recovery
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --model|-m)
            MODEL="$2"
            shift 2
            ;;
        --ksu|-k)
            KSU_OPTION="$2"
            shift 2
            ;;
        --recovery|-r)
            RECOVERY_OPTION="$2"
            shift 2
            ;;
        *)\
            unset_flags
            exit 1
            ;;
    esac
done

echo "Preparing the build environment..."

pushd $(dirname "$0") > /dev/null
CORES=`cat /proc/cpuinfo | grep -c processor`

# Define toolchain variables
CLANG_DIR=$PWD/toolchain/clang-r547379
PATH=$CLANG_DIR/bin:$PATH

# Check if toolchain exists
if [ ! -f "$CLANG_DIR/bin/clang-20" ]; then
    echo "-----------------------------------------------"
    echo "Toolchain not found! Downloading..."
    echo "-----------------------------------------------"
    rm -rf $CLANG_DIR
    mkdir -p $CLANG_DIR
    pushd $CLANG_DIR > /dev/null
    curl -LJOk https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86/+archive/refs/heads/main/clang-r547379.tar.gz
    tar xf main-clang-r547379.tar.gz
    rm main-clang-r547379.tar.gz
    echo "Cleaning up..."
    popd > /dev/null
fi

MAKE_ARGS="
LLVM=1 \
LLVM_IAS=1 \
ARCH=arm64 \
O=out
"

# Define specific variables
case $MODEL in
beyond0lte)
    BOARD=SRPRI28A016KU
    SOC=exynos9820
;;
beyond1lte)
    BOARD=SRPRI28B016KU
    SOC=exynos9820
;;
beyond2lte)
    BOARD=SRPRI17C016KU
    SOC=exynos9820
;;
beyondx)
    BOARD=SRPSC04B014KU
    SOC=exynos9820
;;
d1)
    BOARD=SRPSD26B009KU
    SOC=exynos9825
;;
d1xks)
    BOARD=SRPSD23A002KU
    SOC=exynos9825
;;
d2s)
    BOARD=SRPSC14B009KU
    SOC=exynos9825
;;
d2x)
    BOARD=SRPSC14C009KU
    SOC=exynos9825
;;
d2xks)
    BOARD=SRPSD23C002KU
    SOC=exynos9825
;;
*)
    unset_flags
    exit
esac

if [[ "$MODEL" == "d2xks" ]]; then
    MODEL=d2x
fi

if [[ "$RECOVERY_OPTION" == "y" ]]; then
    RECOVERY=recovery.config
    KSU_OPTION=n
fi

if [ -z $KSU_OPTION ]; then
    read -p "Include KernelSU (y/N): " KSU_OPTION
fi

if [[ "$KSU_OPTION" == "y" ]]; then
    KSU=ksu.config
fi

rm -rf build/out/$MODEL
mkdir -p build/out/$MODEL/zip/files
mkdir -p build/out/$MODEL/zip/META-INF/com/google/android

# Build kernel image
echo "-----------------------------------------------"
echo "Defconfig: "$KERNEL_DEFCONFIG""

if [ -z "$KSU" ]; then
    echo "KSU: No"
else
    echo "KSU: Yes"
fi

if [ -z "$RECOVERY" ]; then
    echo "Recovery: N"
else
    echo "Recovery: Y"
fi

echo "-----------------------------------------------"
echo "Building kernel using "$KERNEL_DEFCONFIG""
echo "Generating configuration file..."
echo "-----------------------------------------------"
make ${MAKE_ARGS} -j$CORES exynos9820_defconfig $MODEL.config $KSU $RECOVERY || abort

echo "Building kernel..."
echo "-----------------------------------------------"
make ${MAKE_ARGS} -j$CORES || abort

# Define constant variables
KERNEL_PATH=build/out/$MODEL/Image
KERNEL_OFFSET=0x00008000
RAMDISK_OFFSET=0xF0000000
SECOND_OFFSET=0xF0000000
TAGS_OFFSET=0x00000100
BASE=0x10000000
CMDLINE='loop.max_part=7'
HASHTYPE=sha1
HEADER_VERSION=1
OS_PATCH_LEVEL=2025-08
OS_VERSION=16.0.0
PAGESIZE=2048
RAMDISK=build/out/$MODEL/ramdisk.cpio.gz
OUTPUT_FILE=build/out/$MODEL/boot.img

## Build auxiliary boot.img files
# Copy kernel to build
cp out/arch/arm64/boot/Image build/out/$MODEL

echo "-----------------------------------------------"
# Build dtb
if [[ "$SOC" == "exynos9820" ]]; then
    echo "Building common exynos9820 Device Tree Blob Image..."
    echo "-----------------------------------------------"
    ./toolchain/mkdtimg cfg_create build/out/$MODEL/dtb.img build/dtconfigs/exynos9820.cfg -d out/arch/arm64/boot/dts/exynos
fi

if [[ "$SOC" == "exynos9825" ]]; then
    echo "Building common exynos9825 Device Tree Blob Image..."
    echo "-----------------------------------------------"
    ./toolchain/mkdtimg cfg_create build/out/$MODEL/dtb.img build/dtconfigs/exynos9825.cfg -d out/arch/arm64/boot/dts/exynos
fi
echo "-----------------------------------------------"

# Build dtbo
echo "Building Device Tree Blob Output Image for "$MODEL"..."
echo "-----------------------------------------------"
./toolchain/mkdtimg cfg_create build/out/$MODEL/dtbo.img build/dtconfigs/$MODEL.cfg -d out/arch/arm64/boot/dts/samsung
echo "-----------------------------------------------"

if [ -z "$RECOVERY" ]; then
    # Build ramdisk
    echo "Building RAMDisk..."
    echo "-----------------------------------------------"
    pushd build/ramdisk > /dev/null
    find . ! -name . | LC_ALL=C sort | cpio -o -H newc -R root:root | gzip > ../out/$MODEL/ramdisk.cpio.gz || abort
    popd > /dev/null
    echo "-----------------------------------------------"

    # Create boot image
    echo "Creating boot image..."
    echo "-----------------------------------------------"
    ./toolchain/mkbootimg --base $BASE --board $BOARD --cmdline "$CMDLINE" --hashtype $HASHTYPE \
    --header_version $HEADER_VERSION --kernel $KERNEL_PATH --kernel_offset $KERNEL_OFFSET \
    --os_patch_level $OS_PATCH_LEVEL --os_version $OS_VERSION --pagesize $PAGESIZE \
    --ramdisk $RAMDISK --ramdisk_offset $RAMDISK_OFFSET --second_offset $SECOND_OFFSET \
    --tags_offset $TAGS_OFFSET -o $OUTPUT_FILE || abort

    # Build zip
    echo "Building zip..."
    echo "-----------------------------------------------"
    cp build/out/$MODEL/boot.img build/out/$MODEL/zip/files/boot.img
    cp build/out/$MODEL/dtb.img build/out/$MODEL/zip/files/dtb.img
    cp build/out/$MODEL/dtbo.img build/out/$MODEL/zip/files/dtbo.img
    cp build/update-binary build/out/$MODEL/zip/META-INF/com/google/android/update-binary
    cp build/updater-script build/out/$MODEL/zip/META-INF/com/google/android/updater-script

    version=$(grep -o 'CONFIG_LOCALVERSION="[^"]*"' arch/arm64/configs/exynos9820_defconfig | cut -d '"' -f 2)

    version=${version:1}

    if [ "$SOC" == "exynos9825" ]; then
        version="${version}-N10"
    else
        version="${version}-S10"
    fi

    pushd build/out/$MODEL/zip > /dev/null
    DATE=`date +"%d-%m-%Y_%H-%M-%S"`    

    if [[ "$KSU_OPTION" == "y" ]]; then
        NAME="$version"_"$MODEL"_UNOFFICIAL_KSU_"$DATE".zip
    else
        NAME="$version"_"$MODEL"_UNOFFICIAL_"$DATE".zip
    fi
    zip -r ../"$NAME" .
    popd > /dev/null
fi

popd > /dev/null
echo "Build finished successfully!"
