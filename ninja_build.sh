#!/bin/bash

set -e

BUILD_DIR="build"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

########################################
# 参数处理
########################################

BUILD_TYPE="Release"

if [ "$1" == "debug" ]; then
    BUILD_TYPE="Debug"
elif [ "$1" == "release" ]; then
    BUILD_TYPE="Release"
elif [ "$1" == "clean" ]; then
    echo -e "${YELLOW}Cleaning build directory...${NC}"
    rm -rf ${BUILD_DIR}
    echo -e "${GREEN}Clean done.${NC}"
    exit 0
fi

########################################
# 检查 Ninja
########################################

if ! command -v ninja >/dev/null 2>&1; then
    echo -e "${RED}[ERROR] Ninja not found.${NC}"
    exit 1
fi

########################################
# 配置
########################################

echo -e "${YELLOW}Configuring (${BUILD_TYPE})...${NC}"

cmake -S . \
      -B ${BUILD_DIR} \
      -G Ninja \
      -DCMAKE_BUILD_TYPE=${BUILD_TYPE}

########################################
# 编译
########################################

echo -e "${YELLOW}Building...${NC}"

cmake --build ${BUILD_DIR} -j$(nproc)

########################################
# 成功
########################################

echo -e "${GREEN}Build success.${NC}"