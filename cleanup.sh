#!/bin/bash

echo "======================================"
echo "Cleaning VLAN-Aware L2 Switch"
echo "======================================"


#
# Remove kernel module first.
#
if lsmod | grep -q "^l2switch"; then

    echo
    echo "Removing l2switch module..."

    sudo rmmod l2switch

fi


#
# Remove namespaces.
#
echo
echo "Removing namespaces..."

sudo ip netns del pc1 2>/dev/null || true
sudo ip netns del pc2 2>/dev/null || true
sudo ip netns del pc3 2>/dev/null || true


#
# Remove switch interfaces.
#
echo
echo "Removing switch ports..."

sudo ip link del swp0 2>/dev/null || true
sudo ip link del swp1 2>/dev/null || true
sudo ip link del swp2 2>/dev/null || true


echo
echo "======================================"
echo "Cleanup complete."
echo "======================================"