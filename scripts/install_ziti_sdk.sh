#!/bin/bash
set -e

# vcpkg Setup for Ziti SDK dependencies
echo "[4/6] Setting up vcpkg..."
if [ ! -d "$HOME/vcpkg" ]; then
    git clone https://github.com/microsoft/vcpkg.git "$HOME/vcpkg"
    "$HOME/vcpkg/bootstrap-vcpkg.sh"
else
    echo "vcpkg directory already exists, skipping clone."
fi

echo "--- Building OpenZiti C-SDK ---"

mkdir -p ~/repos && cd ~/repos
if [ ! -d "ziti-sdk-c" ]; then
    git clone https://github.com/openziti/ziti-sdk-c.git
fi
cd ziti-sdk-c
git submodule update --init --recursive

mkdir -p build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
         -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_INSTALL_PREFIX=/usr/local

echo "--- Compiling with $(nproc) cores ---"
make -j$(nproc)

echo "--- Deploying Headers and Libs to /usr/local ---"
sudo make install

# Manual deployment of transport dependencies for the ROS 2 linker
sudo cp -r ../includes/ziti /usr/local/include/
sudo cp -r _deps/tlsuv-src/include/tlsuv /usr/local/include/
sudo cp library/libziti.so library/libziti.a _deps/tlsuv-build/libtlsuv.a /usr/local/lib/

# Locate and copy libuv from the vcpkg-specific build folder
VCPKG_LIB_DIR=$(find vcpkg_installed -name "lib" -type d | head -n 1)
sudo cp $VCPKG_LIB_DIR/libuv.a /usr/local/lib/

sudo ldconfig
echo "--- Ziti SDK installation successful ---"
