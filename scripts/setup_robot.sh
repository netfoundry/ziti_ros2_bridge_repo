#!/bin/bash

# Exit immediately if a command exits with a non-zero status
set -e

echo "--- Starting Robot Environment Setup (Jazzy + CycloneDDS) ---"
# 1. Update and install initial requirements
sudo apt update && sudo apt install -y curl gnupg2 lsb-release build-essential git
if [ ! -d "$HOME/ros2_ws" ]; then
    mkdir -p "$HOME/ros2_ws"
fi

# 2. Add ROS 2 Jazzy Repository
echo "[1/6] Configuring ROS 2 Jazzy Repositories..."
sudo curl -sSL https://raw.githubusercontent.com/ros/rosdistro/master/ros.key -o /usr/share/keyrings/ros-archive-keyring.gpg
echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/ros-archive-keyring.gpg] http://packages.ros.org/ros2/ubuntu $(. /etc/os-release && echo $UBUNTU_CODENAME) main" | sudo tee /etc/apt/sources.list.d/ros2.list > /dev/null

# 3. Enable Universe and install the main package list
echo "[2/6] Installing ROS 2, Build Tools, and Dependencies..."
sudo add-apt-repository universe -y
sudo apt update
sudo apt install -y \
    rapidjson-dev \
    python3-colcon-common-extensions \
    python3-rosdep \
    python3-vcstool \
    ros-jazzy-rclcpp \
    ros-jazzy-geometry-msgs \
    ros-jazzy-sensor-msgs \
    ros-jazzy-rmw-cyclonedds-cpp \
    pkg-config \
    cmake \
    ros-jazzy-ros2cli \
    ros-jazzy-ros-base \
    autoconf \
    automake \
    libtool \
    ninja-build \
    libuv1-dev \
    libssl-dev \
    libjson-c-dev \
    python3-pip \
    python3-venv \
    zip

# 4. Initialize rosdep
echo "[3/6] Initializing rosdep..."
if [ ! -f /etc/ros/rosdep/sources.list.d/20-default.list ]; then
    sudo rosdep init
fi
rosdep update


# 6. Environment Configuration (.bashrc)
echo "[6/6] Writing ROS 2 & CycloneDDS Configuration to .bashrc..."

# 7. Create a temporary file for the new bashrc entries
cat << 'EOF' > /tmp/ros2_ziti_env

# --- ROS 2 Core Setup ---
source /opt/ros/jazzy/setup.bash
export ROS2_WS=~/ros2_ws

# --- ROS 2 Jazzy & OpenZiti Bridge Config ---
export ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST # CHANGE TO SUBNET to reach subtending robots
export RMW_IMPLEMENTATION=rmw_cyclonedds_cpp
export VCPKG_ROOT=$HOME/vcpkg
export PATH=$VCPKG_ROOT:$PATH
export ROS_DOMAIN_ID=0

# --- CycloneDDS Bind ---
export CYCLONEDDS_URI='<CycloneDDS>
  <Domain>
    <General>
      <Interfaces>
        <NetworkInterface name="lo"/>
      </Interfaces>
    </General>
  </Domain>
</CycloneDDS>'
EOF

# 8. Append to .bashrc if not already present
if ! grep -q "CYCLONEDDS_URI" ~/.bashrc; then
    cat /tmp/ros2_ziti_env >> ~/.bashrc
    echo "Configuration appended to ~/.bashrc"
else
    echo "Configuration already exists in ~/.bashrc, skipping."
fi

rm /tmp/ros2_ziti_env

echo "--- Setup Complete! ---"
echo "Please run 'source ~/.bashrc' or restart your terminal."
