#!/bin/bash
#=========================================================
# 正点原子 I.MX6ULL 开发板 Linux 内核编译脚本
# 使用 Linaro GCC 4.9.4 交叉编译器（无需 Yocto SDK）
# 替代依赖 Yocto SDK 的 build.sh
# 用法: ./gengtao_build_linux_kernel.sh [all|build|zImage]  默认 all
#   all     完整编译（distclean + defconfig + zImage + 板载屏幕对应
#           dtb + modules），产物打包到 build_linux_kernel_output/ 目录
#   build   增量编译（跳过 distclean，复用已有 .config）
#   zImage  仅编译内核镜像（快速验证代码改动）
#=========================================================

set -e  # 任一命令出错即退出

#------------------------ 可修改配置项 --------------------------------
TOOLCHAIN_DIR=/usr/local/arm/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabihf
CROSS_COMPILE=${TOOLCHAIN_DIR}/bin/arm-linux-gnueabihf-
JOBS=$(nproc)                                                  # 并行编译核数
OUTPUT_DIR=./build_linux_kernel_output                         # 编译产物输出目录
DTB=imx6ull-14x14-emmc-7-1024x600-c                           # 板载屏幕对应设备树（只编译这一个）
#----------------------------------------------------------------------

# 切换到脚本所在目录（内核源码根目录）
cd "$(dirname "$0")"

MODE=${1:-all}
MAKE="make ARCH=arm CROSS_COMPILE=${CROSS_COMPILE} -j${JOBS}"

usage()
{
    echo "用法: $0 [all|build|zImage]"
    echo "  all     完整编译并打包到 build_linux_kernel_output/（默认）"
    echo "  build   增量编译（跳过 distclean）并打包到 build_linux_kernel_output/"
    echo "  zImage  仅编译内核镜像"
    exit 1
}

case "$MODE" in
    all|build|zImage) ;;
    *) usage ;;
esac

echo "=============================================="
echo " i.MX6ULL Linux 内核编译脚本"
echo "   交叉编译 : ${CROSS_COMPILE}"
echo "   并行任务 : ${JOBS}"
echo "   模式     : ${MODE}"
echo "=============================================="

# 1. 检查交叉编译器
if [ ! -x "${CROSS_COMPILE}gcc" ]; then
    echo "[错误] 未找到交叉编译器: ${CROSS_COMPILE}gcc"
    echo "       请确认 Linaro GCC 4.9.4 已安装到 ${TOOLCHAIN_DIR}"
    echo "       （安装包位于 /home/gengtao/linux-imx6ull/tool/ 下）"
    exit 1
fi
echo "[1/6] 交叉编译器检查通过: $(${CROSS_COMPILE}gcc --version | head -1)"

# 2. 清理
if [ "${MODE}" == "all" ]; then
    echo "[2/6] make distclean (清理)"
    make distclean > /dev/null
else
    echo "[2/6] 增量模式, 跳过 distclean"
fi

# 3. 配置
if [ "${MODE}" == "all" ] || [ ! -f .config ]; then
    echo "[3/6] make imx_v7_defconfig (生成默认配置)"
    make ARCH=arm CROSS_COMPILE=${CROSS_COMPILE} imx_v7_defconfig > /dev/null
else
    echo "[3/6] 复用已有 .config"
fi

# 4. 编译内核镜像
echo "[4/6] 编译 zImage ..."
${MAKE} zImage

if [ "${MODE}" == "zImage" ]; then
    echo ""
    echo ">> zImage 编译完成: arch/arm/boot/zImage"
    exit 0
fi

# 5. 编译设备树（板载屏幕对应的那一个）
echo "[5/6] 编译设备树 (${DTB}) ..."
${MAKE} ${DTB}.dtb

# 6. 编译内核模块并打包到 build_linux_kernel_output/
echo "[6/6] 编译内核模块 (make modules) ..."
${MAKE} modules

mkdir -p "${OUTPUT_DIR}"
rm -rf "${OUTPUT_DIR}"/*
make modules_install INSTALL_MOD_PATH="${OUTPUT_DIR}"
( cd "${OUTPUT_DIR}"/lib/modules && tar -jcvf ../../modules.tar.bz2 . )
rm -rf "${OUTPUT_DIR}"/lib
cp arch/arm/boot/zImage "${OUTPUT_DIR}"/
cp arch/arm/boot/dts/"${DTB}".dtb "${OUTPUT_DIR}"/

echo ""
echo "=============================================="
echo " 编译完成! 产物位于 ${OUTPUT_DIR}/:"
ls "${OUTPUT_DIR}"
echo ""
echo " 烧录使用: ${OUTPUT_DIR}/zImage + ${DTB}.dtb + modules.tar.bz2"
echo " 验证命令:"
echo "   file arch/arm/boot/zImage    # 应显示 ARM boot executable"
echo "=============================================="
