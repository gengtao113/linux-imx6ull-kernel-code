#!/bin/bash
#=========================================================
# 正点原子 I.MX6ULL 开发板 Linux 内核更新脚本
# 把编译产物更新到 TFTP / NFS 服务目录，板子复位即可验证
# 用法: ./gengtao_update_linux_kernel.sh [tftp|nfs|all]  默认 tftp
#   tftp  更新 zImage + dtb 到 TFTP 目录（最快验证）
#   nfs   更新内核模块到 NFS 根文件系统
#   all   TFTP + NFS 全部更新
#=========================================================

set -e  # 任一命令出错即退出

#------------------------ 可修改配置项 --------------------------------
OUTPUT_DIR=./build_linux_kernel_output                         # 编译产物目录
TFTP_DIR=/home/gengtao/linux-imx6ull/tftpboot                  # TFTP 目录（uboot 从此拉取内核）
ROOTFS_DIR=/home/gengtao/linux-imx6ull/nfs/rootfs              # NFS 根文件系统（更新模块用）
DTB=imx6ull-14x14-emmc-7-1024x600-c                           # 板载屏幕对应设备树
#----------------------------------------------------------------------

# 切换到脚本所在目录（内核源码根目录）
cd "$(dirname "$0")"

MODE=${1:-tftp}

usage()
{
    echo "用法: $0 [tftp|nfs|all]"
    echo "  tftp  更新 zImage + dtb 到 TFTP 目录（默认）"
    echo "  nfs   更新内核模块到 NFS 根文件系统"
    echo "  all   TFTP + NFS 全部更新"
    exit 1
}

case "$MODE" in
    tftp|nfs|all) ;;
    *) usage ;;
esac

echo "=============================================="
echo " i.MX6ULL Linux 内核更新脚本"
echo "   产物目录 : ${OUTPUT_DIR}"
echo "   模式     : ${MODE}"
echo "=============================================="

# 1. 更新 TFTP（zImage + dtb）
if [ "${MODE}" == "tftp" ] || [ "${MODE}" == "all" ]; then
    if [ ! -f "${OUTPUT_DIR}/zImage" ] || [ ! -f "${OUTPUT_DIR}/${DTB}.dtb" ]; then
        echo "[错误] 未找到编译产物，请先执行 ./gengtao_build_linux_kernel.sh"
        exit 1
    fi
    if [ ! -d "${TFTP_DIR}" ]; then
        echo "[错误] TFTP 目录不存在: ${TFTP_DIR}"
        exit 1
    fi
    echo "-- 更新 TFTP 目录: ${TFTP_DIR} --"
    cp -v "${OUTPUT_DIR}/zImage" "${TFTP_DIR}/"
    cp -v "${OUTPUT_DIR}/${DTB}.dtb" "${TFTP_DIR}/"
fi

# 2. 更新 NFS（内核模块）
if [ "${MODE}" == "nfs" ] || [ "${MODE}" == "all" ]; then
    if [ ! -f "${OUTPUT_DIR}/modules.tar.bz2" ]; then
        echo "[错误] 未找到 ${OUTPUT_DIR}/modules.tar.bz2"
        exit 1
    fi
    echo "-- 更新 NFS 根文件系统: ${ROOTFS_DIR} --"
    tar -jxf "${OUTPUT_DIR}/modules.tar.bz2" -C "${ROOTFS_DIR}"
fi

echo ""
echo "=============================================="
echo " 更新完成! 复位板子后依次执行验证:"
echo "   uname -a"
echo "   cat /sys/class/graphics/fb0/virtual_size"
echo "=============================================="
