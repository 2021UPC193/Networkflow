
# meson file, for building this example as part of a main DPDK build.
#
# To build this example as a standalone application with an already-installed
# DPDK instance, use 'make'

project('NetworkFlow', 'c')

# 查找 DPDK 依赖
dpdk_dep = dependency('libdpdk', required: true)

# 可执行文件
executable('dpdk_stats',
    'main.c',
    dependencies: dpdk_dep,
    install: false
)