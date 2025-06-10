#!/bin/bash
set -x
set -e
##############################
GITHUB_WORKSPACE="${PWD}"
ls -la ${GITHUB_WORKSPACE}

############################
# Build dobby
echo "======================================================================================"
echo "building dobby"

cd $GITHUB_WORKSPACE
git clone https://github.com/containers/libocispec.git
cd libocispec
git checkout 9d1c2253a4098349749b7a60772ac3ec74338d5b
cd ..
mkdir build
cd build
cmake -DRDK_PLATFORM=DEV_VM -DCMAKE_INSTALL_PREFIX:PATH=/usr -DCMAKE_BUILD_TYPE=Debug -DLEGACY_COMPONENTS=ON -DPLUGIN_TESTPLUGIN=ON -DPLUGIN_GPU=ON -DPLUGIN_LOCALTIME=ON -DPLUGIN_RTSCHEDULING=ON -DPLUGIN_HTTPPROXY=ON -DPLUGIN_APPSERVICES=ON -DPLUGIN_IONMEMORY=ON -DPLUGIN_DEVICEMAPPER=ON -DPLUGIN_OOMCRASH=ON -DLEGACY_COMPONENTS=ON -DRDK=ON -DUSE_SYSTEMD=ON -DDOBBY_HIBERNATE_MEMCR_IMPL=ON -DDOBBY_HIBERNATE_MEMCR_PARAMS_ENABLED=ON ..
make -j $(nproc)
