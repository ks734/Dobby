#!/bin/bash
set -e
set -x
##############################
GITHUB_WORKSPACE="${PWD}"
ls -la ${GITHUB_WORKSPACE}
cd ${GITHUB_WORKSPACE}

# # ############################# 
#1. Install Dependencies and packages

apt update
apt-get install -q -y automake libtool autotools-dev software-properties-common build-essential cmake libsystemd-dev libctemplate-dev libjsoncpp-dev libdbus-1-dev libnl-3-dev libnl-route-3-dev libsystemd-dev libyajl-dev libcap-dev libboost-dev lcov clang valgrind meson libyaml-dev
pip3 install xmltodict
pip3 install requests
############################
git clone https://github.com/containers/libocispec.git
cd libocispec
git checkout 9d1c2253a4098349749b7a60772ac3ec74338d5b

meson build
ninja -C build
sudo ninja -C build install
cd -
