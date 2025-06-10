#!/bin/bash
set -x
set -e
##############################
GITHUB_WORKSPACE="${PWD}"
ls -la ${GITHUB_WORKSPACE}

echo "TOOLCHAIN_FILE=${GITHUB_WORKSPACE}/tests/clang.cmake" >> $GITHUB_ENV
echo "TOOLCHAIN_FILE=${GITHUB_WORKSPACE}/tests/gcc-with-coverage.cmake" >> $GITHUB_ENV

############################
# Build dobby
echo "======================================================================================"
echo "building dobby"

cd $GITHUB_WORKSPACE
mkdir build
cd build
cmake -DCMAKE_TOOLCHAIN_FILE="${{ TOOLCHAIN_FILE }}" -DRDK_PLATFORM=DEV_VM -DCMAKE_INSTALL_PREFIX:PATH=/usr -DCMAKE_BUILD_TYPE=Debug -DPLUGIN_TESTPLUGIN=ON -DPLUGIN_GPU=ON -DPLUGIN_LOCALTIME=ON -DPLUGIN_RTSCHEDULING=ON -DPLUGIN_HTTPPROXY=ON -DPLUGIN_APPSERVICES=ON -DPLUGIN_IONMEMORY=ON -DPLUGIN_DEVICEMAPPER=ON -DPLUGIN_OOMCRASH=ON -DLEGACY_COMPONENTS=ON -DRDK=ON -DUSE_SYSTEMD=ON -DDOBBY_HIBERNATE_MEMCR_IMPL=ON -DDOBBY_HIBERNATE_MEMCR_PARAMS_ENABLED=ON ..
make -j $(nproc)
