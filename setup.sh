#!/bin/bash

set -e

echo "======================================"
echo "Creating L2 switch test topology"
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


echo "[1/6] Creating namespaces..."

ip netns add pc1
ip netns add pc2
ip netns add pc3


echo "[2/6] Creating veth pairs..."

ip link add swp0 type veth peer name pc1eth
ip link add swp1 type veth peer name pc2eth
ip link add swp2 type veth peer name pc3eth


echo "[3/6] Moving peer interfaces into namespaces..."

ip link set pc1eth netns pc1
ip link set pc2eth netns pc2
ip link set pc3eth netns pc3


echo "[4/6] Configuring namespace interfaces..."

ip netns exec pc1 ip link set pc1eth name eth0
ip netns exec pc2 ip link set pc2eth name eth0
ip netns exec pc3 ip link set pc3eth name eth0


#
# Assign IP addresses.
#
ip netns exec pc1 ip addr add 10.0.0.1/24 dev eth0
ip netns exec pc2 ip addr add 10.0.0.2/24 dev eth0
ip netns exec pc3 ip addr add 10.0.0.3/24 dev eth0


echo "[5/6] Bringing interfaces UP..."

#
# Namespace interfaces.
#
ip netns exec pc1 ip link set lo up
ip netns exec pc2 ip link set lo up
ip netns exec pc3 ip link set lo up

ip netns exec pc1 ip link set eth0 up
ip netns exec pc2 ip link set eth0 up
ip netns exec pc3 ip link set eth0 up


#
# Switch ports.
#
ip link set swp0 up
ip link set swp1 up
ip link set swp2 up


echo "[6/6] Verifying topology..."

echo
echo "Switch ports:"
ip link show swp0
ip link show swp1
ip link show swp2

echo
echo "Namespaces:"
ip netns list

echo
echo "PC1:"
ip netns exec pc1 ip addr show eth0

echo
echo "PC2:"
ip netns exec pc2 ip addr show eth0

echo
echo "PC3:"
ip netns exec pc3 ip addr show eth0

echo
echo "======================================"
echo "Topology ready!"
echo "======================================"
echo
echo "PC1 = 10.0.0.1"
echo "PC2 = 10.0.0.2"
echo "PC3 = 10.0.0.3"
echo
echo "Load the module with:"
echo "  sudo insmod l2switch.ko"
echo
