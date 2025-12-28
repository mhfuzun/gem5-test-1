#!/bin/bash
# disk_losetup.sh

DISK_IMAGE="riscv-disk.img"
MOUNT_POINT="mnt"

# Check if hello exists
if [ ! -f "./benchs/hello_world/hello" ]; then
    echo "Error: hello binary not found."
    echo "Compile with: riscv64-linux-gnu-gcc -static hello.c -o hello"
    exit 1
fi

# Check if disk image exists
if [ ! -f "${DISK_IMAGE}" ]; then
    echo "Downloading disk image..."
    wget http://dist.gem5.org/dist/develop/images/riscv/busybox/riscv-disk.img.gz
    gunzip riscv-disk.img.gz
fi

# Create mount point
mkdir -p ${MOUNT_POINT}

# Use losetup to handle partitions automatically
echo "Setting up loop device..."
LOOP_DEVICE=$(sudo losetup -f --show -P ${DISK_IMAGE})

if [ -z "${LOOP_DEVICE}" ]; then
    echo "Error: Failed to create loop device"
    exit 1
fi

echo "Loop device created: ${LOOP_DEVICE}"

# Wait a moment for partition devices to appear
sleep 1

# List available partition devices
echo "Available devices:"
ls -l ${LOOP_DEVICE}* 2>/dev/null || ls -l ${LOOP_DEVICE}

# Try to mount the first partition
if [ -e "${LOOP_DEVICE}p1" ]; then
    PART_DEVICE="${LOOP_DEVICE}p1"
elif [ -e "${LOOP_DEVICE}1" ]; then
    PART_DEVICE="${LOOP_DEVICE}1"
else
    PART_DEVICE="${LOOP_DEVICE}"
fi

echo "Mounting ${PART_DEVICE}..."
if sudo mount ${PART_DEVICE} ${MOUNT_POINT}; then
    echo "✓ Mount successful!"

    # Copy hello binary
    sudo mkdir -p ${MOUNT_POINT}/root
    sudo cp hello ${MOUNT_POINT}/root/
    sudo chmod +x ${MOUNT_POINT}/root/hello

    echo ""
    echo "Contents of /root:"
    sudo ls -la ${MOUNT_POINT}/root/

    # Unmount and cleanup
    sudo umount ${MOUNT_POINT}
    sudo losetup -d ${LOOP_DEVICE}

    echo ""
    echo "✓ Disk preparation complete!"
else
    echo "Error: Mount failed"
    sudo losetup -d ${LOOP_DEVICE}
    exit 1
fi
