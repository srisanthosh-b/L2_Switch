# Mini L2 Switch

A simple Linux kernel module that implements a miniature Layer-2 Ethernet switch using Netfilter hooks and a MAC address table.

## Features

- Linux kernel module-based L2 switching behavior
- MAC learning and forwarding
- Broadcast and unknown-unicast flooding
- Per-port statistics exposure through procfs
- Sample virtual topology setup with network namespaces and veth pairs

## Project files

- `l2switch.c` — kernel switch implementation
- `Makefile` — builds the kernel module
- `setup.sh` — creates a 3-host veth test topology
- `switchctl.sh` — views learned MAC addresses and switch stats
- `cleanup.sh` — removes namespaces and switch interfaces

## Requirements

- Linux kernel headers for your running kernel
- Root privileges (`sudo`)
- A Linux system with `ip`, `ip netns`, and kernel module support

## Build

```bash
make
```

## Create the test topology

```bash
sudo ./setup.sh
```

## Load the module

```bash
sudo insmod l2switch.ko
```

## View switch data

```bash
sudo ./switchctl.sh mac
sudo ./switchctl.sh stats
```

## Remove topology and module

```bash
sudo ./cleanup.sh
```

## Notes

This project is intended for learning and experimentation in a Linux networking environment. It is not a production-ready switch implementation.

## GitHub upload

Initialize a Git repository and push it to your GitHub account:

```bash
git init
git add .
git commit -m "Initial commit"
git branch -M main
git remote add origin <your-github-repo-url>
git push -u origin main
```

If you want to keep generated build artifacts out of Git, the included `.gitignore` file already excludes them.
