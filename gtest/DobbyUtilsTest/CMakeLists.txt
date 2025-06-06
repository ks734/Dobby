# If not stated otherwise in this file or this component's LICENSE file the
# following copyright and licenses apply:
#
# Copyright 2020 Sky UK
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.7)
project(DobbyUtilsTest)

set(CMAKE_CXX_STANDARD 11)

include(CTest)
find_package(GTest REQUIRED)
include_directories(${GTEST_INCLUDE_DIRS})

add_library(Utils SHARED
            STATIC
            ../../utils/source/DobbyUtils.cpp
            ../../utils/source/ContainerId.cpp
            ../../utils/source/DobbyTimer.cpp
            ../../AppInfrastructure/Logging/source/Logging.cpp
            )

target_include_directories(Utils
                PUBLIC
                ../../utils/include
                ../../utils/source
                ../../AppInfrastructure/Logging/include
                ../../AppInfrastructure/Common/include
                )

file(GLOB TESTS tests/*.cpp)

add_executable(utilsTest ${TESTS})
add_test(NAME AllTestsInUtils COMMAND utilsTest)

target_link_libraries(utilsTest Utils ${GTEST_LIBRARIES} gtest_main pthread)

install(TARGETS utilsTest DESTINATION bin)
