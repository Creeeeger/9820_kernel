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
    -g, --gpu-max [value]   Set GPU max MHz (ex: 806) using forOC tables (default: 702)
EOF
}

apply_gpu_tables()
{
    local max_khz="$1"
    local src_dtsi="$PWD/forOC/exynos9820-mali_tables.dtsi"
    local dst_dtsi="$PWD/arch/arm64/boot/dts/exynos/exynos9820-mali_tables.dtsi"
    local src_cal="$PWD/forOC/g3d_dvfs_table.h"
    local dst_cal="$PWD/drivers/soc/samsung/cal-if/g3d_dvfs_table.h"

    if [ ! -f "$src_dtsi" ] || [ ! -f "$src_cal" ]; then
        echo "GPU table sources not found under forOC; skipping GPU table update."
        return 1
    fi

    if ! command -v python3 >/dev/null 2>&1; then
        echo "python3 not found; cannot update GPU tables."
        return 1
    fi

    python3 - "$max_khz" "$src_dtsi" "$dst_dtsi" "$src_cal" "$dst_cal" << 'PY'
import re
import sys
from pathlib import Path

max_khz = int(sys.argv[1])
src_dtsi = Path(sys.argv[2])
dst_dtsi = Path(sys.argv[3])
src_cal = Path(sys.argv[4])
dst_cal = Path(sys.argv[5])

def parse_table(lines, key):
    start = next(i for i, l in enumerate(lines) if key in l)
    end = next(i for i in range(start + 1, len(lines)) if ">;" in lines[i])
    entries = []
    indent = None
    for line in lines[start + 1:end + 1]:
        stripped = line.strip()
        if not stripped or stripped.startswith("/*"):
            continue
        cleaned = re.sub(r">;\s*$", "", stripped)
        cleaned = re.sub(r">\s*$", "", cleaned)
        parts = cleaned.split()
        if not parts or not parts[0].isdigit():
            continue
        freq = int(parts[0])
        if indent is None:
            indent = line[:len(line) - len(line.lstrip())]
        entries.append((freq, cleaned))
    if indent is None:
        indent = "\t"
    return start, end, entries, indent

def update_size_line(line, row, cols):
    return re.sub(r"<\s*\d+\s+%d\s*>" % cols, f"<{row} {cols}>", line)

def update_clock_line(line, value):
    return re.sub(r"<\s*\d+\s*>", f"<{value}>", line)

def write_dtsi():
    lines = src_dtsi.read_text().splitlines(True)
    start, end, entries, indent = parse_table(lines, "gpu_dvfs_table = <")
    filtered = [e for e in entries if e[0] <= max_khz]
    if not any(e[0] == max_khz for e in filtered):
        raise SystemExit(f"GPU max {max_khz} not found in DVFS table")
    dvfs_block = []
    for idx, (_, cleaned) in enumerate(filtered):
        suffix = " >;\n" if idx == len(filtered) - 1 else "\n"
        dvfs_block.append(f"{indent}{cleaned}{suffix}")
    lines[start + 1:end + 1] = dvfs_block

    start, end, entries, indent = parse_table(lines, "gpu_cl_pmqos_table = <")
    filtered_cl = [e for e in entries if e[0] <= max_khz]
    if not any(e[0] == max_khz for e in filtered_cl):
        raise SystemExit(f"GPU max {max_khz} not found in PMQoS table")
    cl_block = []
    for idx, (_, cleaned) in enumerate(filtered_cl):
        suffix = " >;\n" if idx == len(filtered_cl) - 1 else "\n"
        cl_block.append(f"{indent}{cleaned}{suffix}")
    lines[start + 1:end + 1] = cl_block

    for i, line in enumerate(lines):
        if line.strip().startswith("gpu_dvfs_table_size"):
            lines[i] = update_size_line(line, len(filtered), 8)
        elif line.strip().startswith("gpu_cl_pmqos_table_size"):
            lines[i] = update_size_line(line, len(filtered_cl), 5)
        elif line.strip().startswith("gpu_max_clock_limit"):
            lines[i] = update_clock_line(line, max_khz)
        elif line.strip().startswith("gpu_max_clock"):
            lines[i] = update_clock_line(line, max_khz)

    dst_dtsi.write_text("".join(lines))

def write_cal():
    lines = src_cal.read_text().splitlines(True)
    start = next(i for i, l in enumerate(lines)
                 if "G3D_DVFS_TABLE_ENTRY_LIST" in l)
    i = start + 1
    entries = []
    indent = None
    while i < len(lines):
        line = lines[i]
        if line.strip().startswith("#endif") or not line.strip():
            break
        m = re.search(r"\bX\((\d+),", line)
        if m:
            if indent is None:
                indent = line[:len(line) - len(line.lstrip())]
            entries.append((int(m.group(1)), line))
        i += 1
    if indent is None:
        indent = "\t"
    filtered = [(f, l) for (f, l) in entries if f <= max_khz]
    if not any(f == max_khz for f, _ in filtered):
        raise SystemExit(f"GPU max {max_khz} not found in CAL table")
    new_list = []
    for idx, (_, line) in enumerate(filtered):
        core = line.rstrip().rstrip("\\").rstrip()
        suffix = "\n" if idx == len(filtered) - 1 else "                               \\\n"
        new_list.append(f"{indent}{core}{suffix}")
    lines[start + 1:i] = new_list
    dst_cal.write_text("".join(lines))

write_dtsi()
write_cal()
PY
}

DEFAULT_GPU_MAX_BEYOND=702
DEFAULT_GPU_MAX_D=754

while [[ $# -gt 0 ]]; do
    case "$1" in
        --model|-m)
            MODEL="$2"
            shift 2
            ;;
        --gpu-max|-g)
            GPU_MAX="$2"
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
        *)
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

MAKE_ARGS=(
    LLVM=1
    LLVM_IAS=1
    ARCH=arm64
    O=out
)

# Prefer content-addressed compiler cache so rebuilds in fresh checkouts are fast
if command -v ccache >/dev/null 2>&1; then
    echo "ccache detected, enabling compiler cache..."
    export CCACHE_DIR="${CCACHE_DIR:-$PWD/.ccache}"
    export CCACHE_BASEDIR="${CCACHE_BASEDIR:-$PWD}"
    export CCACHE_COMPILERCHECK="${CCACHE_COMPILERCHECK:-content}"
    export CCACHE_NOHASHDIR="${CCACHE_NOHASHDIR:-1}"
    export CCACHE_SLOPPINESS="${CCACHE_SLOPPINESS:-file_macro,locale,time_macros}"
    MAKE_ARGS+=(CC="ccache clang" HOSTCC="ccache clang" HOSTCXX="ccache clang++")
else
    echo "ccache not found, building without compiler cache."
fi

# Define specific variables
case $MODEL in
beyond0lte)
    BOARD=SRPRI28A016KU
    SOC=exynos9820
;;
beyond0lteks)
    BOARD=SRPRI28C007KU
    SOC=exynos9820
;;
beyond1lte)
    BOARD=SRPRI28B016KU
    SOC=exynos9820
;;
beyond1lteks)
    BOARD=SRPRI28D007KU
    SOC=exynos9820
;;
beyond2lte)
    BOARD=SRPRI17C016KU
    SOC=exynos9820
;;
beyond2lteks)
    BOARD=SRPRI28E007KU
    SOC=exynos9820
;;
beyondx)
    BOARD=SRPSC04B014KU
    SOC=exynos9820
;;
beyondxks)
    BOARD=SRPRK21D006KU
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

if [ -z "$GPU_MAX" ]; then
    if [[ "$MODEL" == d* ]]; then
        GPU_MAX=$DEFAULT_GPU_MAX_D
    else
        GPU_MAX=$DEFAULT_GPU_MAX_BEYOND
    fi
fi

if [ -n "$GPU_MAX" ]; then
    if ! [[ "$GPU_MAX" =~ ^[0-9]+$ ]]; then
        echo "Invalid GPU max value: $GPU_MAX"
        exit 1
    fi
        if [ "$GPU_MAX" -lt 10000 ]; then
            GPU_MAX_KHZ=$((GPU_MAX * 1000))
        else
            GPU_MAX_KHZ=$GPU_MAX
        fi
        echo "Applying GPU max: ${GPU_MAX_KHZ} kHz (from forOC tables)"
        apply_gpu_tables "$GPU_MAX_KHZ" || abort
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
make "${MAKE_ARGS[@]}" -j$CORES exynos9820_defconfig $MODEL.config $KSU $RECOVERY || abort

echo "Building kernel..."
echo "-----------------------------------------------"
make "${MAKE_ARGS[@]}" -j$CORES || abort

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
        NAME="$version"_"$MODEL"_OFFICIAL_KSU_"$DATE".zip
    else
        NAME="$version"_"$MODEL"_OFFICIAL_"$DATE".zip
    fi

    # Store the generated archive inside the zip directory so CI artifact
    # uploads can glob the file reliably.
    zip -r "$NAME" .
    popd > /dev/null
fi

popd > /dev/null
echo "Build finished successfully!"
