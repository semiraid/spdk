#
#  BSD LICENSE
#
#  Copyright (c) Intel Corporation.
#  All rights reserved.
#  Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#
#  Redistribution and use in source and binary forms, with or without
#  modification, are permitted provided that the following conditions
#  are met:
#
#    * Redistributions of source code must retain the above copyright
#      notice, this list of conditions and the following disclaimer.
#    * Redistributions in binary form must reproduce the above copyright
#      notice, this list of conditions and the following disclaimer in
#      the documentation and/or other materials provided with the
#      distribution.
#    * Neither the name of Intel Corporation nor the names of its
#      contributors may be used to endorse or promote products derived
#      from this software without specific prior written permission.
#
#  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
#  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
#  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
#  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
#  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
#  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
#  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
#  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
#  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
#  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
#  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

# Installation prefix
CONFIG[PREFIX]="/usr/local"

# Target architecture
CONFIG[ARCH]=native

# Prefix for cross compilation
CONFIG[CROSS_PREFIX]=

# Build with debug logging. Turn off for performance testing and normal usage
CONFIG[DEBUG]=n

# Treat warnings as errors (fail the build on any warning).
CONFIG[WERROR]=n

# Build with link-time optimization.
CONFIG[LTO]=n

# Generate profile guided optimization data.
CONFIG[PGO_CAPTURE]=n

# Use profile guided optimization data.
CONFIG[PGO_USE]=n

# Build with code coverage instrumentation.
CONFIG[COVERAGE]=n

# Build with Address Sanitizer enabled
CONFIG[ASAN]=n

# Build with Undefined Behavior Sanitizer enabled
CONFIG[UBSAN]=n

# Build with Thread Sanitizer enabled
CONFIG[TSAN]=n

# Build functional tests
CONFIG[TESTS]=y

# Build unit tests
CONFIG[UNIT_TESTS]=y

# Build examples
CONFIG[EXAMPLES]=y

# Build apps
CONFIG[APPS]=y

# Build with Control-flow Enforcement Technology (CET)
CONFIG[CET]=n

# Directory that contains the desired SPDK environment library.
# By default, this is implemented using DPDK.
CONFIG[ENV]=

# This directory should contain 'include' and 'lib' directories for your DPDK
# installation.
CONFIG[DPDK_DIR]=
# Automatically set via pkg-config when bare --with-dpdk is set
CONFIG[DPDK_LIB_DIR]=
CONFIG[DPDK_INC_DIR]=
CONFIG[DPDK_PKG_CONFIG]=n

# This directory should contain 'include' and 'lib' directories for WPDK.
CONFIG[WPDK_DIR]=

# Build SPDK FIO plugin. Requires CONFIG_FIO_SOURCE_DIR set to a valid
# fio source code directory.
CONFIG[FIO_PLUGIN]=n

# This directory should contain the source code directory for fio
# which is required for building the SPDK FIO plugin.
CONFIG[FIO_SOURCE_DIR]=/usr/src/fio

# Enable RDMA support for the NVMf target.
# Requires ibverbs development libraries.
CONFIG[RDMA]=n
CONFIG[RDMA_SEND_WITH_INVAL]=n
CONFIG[RDMA_SET_ACK_TIMEOUT]=n
CONFIG[RDMA_PROV]=verbs

# Enable NVMe Character Devices.
CONFIG[NVME_CUSE]=n

# Enable FC support for the NVMf target.
# Requires FC low level driver (from FC vendor)
CONFIG[FC]=n
CONFIG[FC_PATH]=

# Build Ceph RBD support in bdev modules
# Requires librbd development libraries
CONFIG[RBD]=n

# Build vhost library.
CONFIG[VHOST]=y

# Build vhost initiator (Virtio) driver.
CONFIG[VIRTIO]=y

# Build custom vfio-user transport for NVMf target and NVMe initiator.
CONFIG[VFIO_USER]=n
CONFIG[VFIO_USER_DIR]=

# Build with PMDK backends
CONFIG[PMDK]=n
CONFIG[PMDK_DIR]=

# Enable the dependencies for building the compress vbdev
CONFIG[REDUCE]=n

# Enable mlx5_pci dpdk compress PMD, enabled automatically if CONFIG[REDUCE]=y and libmlx5 exists
CONFIG[REDUCE_MLX5]=n

# Requires libiscsi development libraries.
CONFIG[ISCSI_INITIATOR]=n

# Enable the dependencies for building the crypto vbdev
CONFIG[CRYPTO]=n

# Build spdk shared libraries in addition to the static ones.
CONFIG[SHARED]=n

# Build with VTune suport.
CONFIG[VTUNE]=n
CONFIG[VTUNE_DIR]=

# Build Intel IPSEC_MB library
CONFIG[IPSEC_MB]=n

# Enable OCF module
CONFIG[OCF]=n
CONFIG[OCF_PATH]=
CONFIG[CUSTOMOCF]=n

# Build ISA-L library
CONFIG[ISAL]=y

# Build with IO_URING support
CONFIG[URING]=n

# Path to custom built IO_URING library
CONFIG[URING_PATH]=

# Build with FUSE support
CONFIG[FUSE]=n

# Build with RAID5 support
CONFIG[RAID5]=n

# Build with IDXD support
# In this mode, SPDK fully controls the DSA device.
CONFIG[IDXD]=n

# Build with USDT support
CONFIG[USDT]=n

# Build with IDXD kernel support.
# In this mode, SPDK shares the DSA device with the kernel.
CONFIG[IDXD_KERNEL]=n
