# haptics 子系统 + TI DRV2605 驱动（从 Zephyr 4.1 移植到 3.5）
# SPDX-License-Identifier: Apache-2.0
#
# 对应 4.1 的 zephyr/drivers/haptics/CMakeLists.txt，只是路径换成本模块。

if(CONFIG_HAPTICS)

# 3.5 的 extensions.cmake 里有这个函数，用它把子系统头注册进 syscall 扫描，
# 否则 include/zephyr/drivers/haptics.h 末尾的 <syscalls/haptics.h> 找不到。
zephyr_syscall_header(${CMAKE_CURRENT_SOURCE_DIR}/include/zephyr/drivers/haptics.h)

zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_HAPTICS_DRV2605 drivers/haptics/drv2605.c)
zephyr_library_sources_ifdef(CONFIG_USERSPACE drivers/haptics/haptics_handlers.c)

endif() # CONFIG_HAPTICS
