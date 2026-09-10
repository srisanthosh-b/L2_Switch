#!/bin/bash

set -e

echo "======================================"
echo "Creating VLAN-Aware L2 Switch Topology"
echo "======================================"

#
# Remove old namespaces if they exist.
#
ip netns del pc1 2>/dev/null || true
ip netns del pc2 2>/dev/null || true
ip netns del pc3 2>/dev/null || true

#
# Remove old switch ports if they exist.
#
ip link del swp0 2>/dev/null || true
ip link del swp1 2>/dev/null || true
ip link del swp2 2>/dev/null || true


echo
echo "[1/6] Creating namespaces..."

ip netns add pc1
ip netns add pc2
ip netns add pc3


echo
echo "[2/6] Creating veth pairs..."

#
# Each pair behaves like a cable.
#
# swp0 <----> pc1eth
# swp1 <----> pc2eth
# swp2 <----> pc3eth
#

ip link add swp0 type veth peer name pc1eth
ip link add swp1 type veth peer name pc2eth
ip link add swp2 type veth peer name pc3eth


echo
echo "[3/6] Moving peer interfaces into namespaces..."

ip link set pc1eth netns pc1
ip link set pc2eth netns pc2
ip link set pc3eth netns pc3


echo
echo "[4/6] Configuring namespace interfaces..."

#
# Rename interfaces to eth0.
#

ip netns exec pc1 ip link set pc1eth name eth0
ip netns exec pc2 ip link set pc2eth name eth0
ip netns exec pc3 ip link set pc3eth name eth0


#
# Assign IP addresses.
#
# All three PCs are deliberately in the same
# IP subnet.
#
# This makes it easy to demonstrate that
# VLAN isolation happens at Layer 2.
#

ip netns exec pc1 ip addr add 10.0.0.1/24 dev eth0
ip netns exec pc2 ip addr add 10.0.0.2/24 dev eth0
ip netns exec pc3 ip addr add 10.0.0.3/24 dev eth0


echo
echo "[5/6] Bringing interfaces UP..."

#
# Loopback interfaces.
#

ip netns exec pc1 ip link set lo up
ip netns exec pc2 ip link set lo up
ip netns exec pc3 ip link set lo up


#
# PC interfaces.
#

ip netns exec pc1 ip link set eth0 up
ip netns exec pc2 ip link set eth0 up
ip netns exec pc3 ip link set eth0 up


#
# Switch ports.
#

ip link set swp0 up
ip link set swp1 up
ip link set swp2 up


echo
echo "[6/6] Verifying topology..."


echo
echo "======================================"
echo "Switch ports"
echo "======================================"

ip link show swp0
ip link show swp1
ip link show swp2


echo
echo "======================================"
echo "VLAN configuration"
echo "======================================"

echo "swp0 -> VLAN 10"
echo "swp1 -> VLAN 10"
echo "swp2 -> VLAN 20"


echo
echo "======================================"
echo "Namespaces"
echo "======================================"

ip netns list


echo
echo "======================================"
echo "PC1"
echo "======================================"

ip netns exec pc1 ip addr show eth0


echo
echo "======================================"
echo "PC2"
echo "======================================"

ip netns exec pc2 ip addr show eth0


echo
echo "======================================"
echo "PC3"
echo "======================================"

ip netns exec pc3 ip addr show eth0


echo
echo "======================================"
echo "Topology ready!"
echo "======================================"

echo
echo "PC1 = 10.0.0.1  -> swp0 -> VLAN 10"
echo "PC2 = 10.0.0.2  -> swp1 -> VLAN 10"
echo "PC3 = 10.0.0.3  -> swp2 -> VLAN 20"

echo
echo "Expected behavior:"
echo
echo "  PC1 -> PC2   : SHOULD WORK"
echo "  PC2 -> PC1   : SHOULD WORK"
echo
echo "  PC1 -> PC3   : SHOULD NOT WORK"
echo "  PC3 -> PC1   : SHOULD NOT WORK"
echo
echo "  PC2 -> PC3   : SHOULD NOT WORK"

echo
echo "Load the module with:"
echo
echo "  sudo insmod l2switch.ko"
echo