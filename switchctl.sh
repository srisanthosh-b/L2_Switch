#!/bin/bash

PROC_DIR="/proc/l2switch"


case "$1" in


    mac)

        echo "======================================"
        echo "MAC ADDRESS TABLE"
        echo "======================================"

        if [ ! -f "$PROC_DIR/mac" ]; then

            echo "Error: l2switch module is not loaded."

            exit 1

        fi

        cat "$PROC_DIR/mac"

        ;;


    stats)

        echo "======================================"
        echo "SWITCH STATISTICS"
        echo "======================================"

        if [ ! -f "$PROC_DIR/stats" ]; then

            echo "Error: l2switch module is not loaded."

            exit 1

        fi

        cat "$PROC_DIR/stats"

        ;;


    clear)

        echo "======================================"
        echo "CLEARING MAC TABLE"
        echo "======================================"

        if [ ! -f "$PROC_DIR/clear" ]; then

            echo "Error: l2switch module is not loaded."

            exit 1

        fi

        #
        # Use tee because:
        #
        # sudo echo 1 > /proc/...
        #
        # does NOT work as expected because
        # shell redirection happens before sudo.
        #
        echo 1 | sudo tee "$PROC_DIR/clear" > /dev/null

        echo "MAC table cleared."

        ;;


    vlan)

        echo "======================================"
        echo "VLAN CONFIGURATION"
        echo "======================================"

        echo
        echo "swp0 -> VLAN 10"
        echo "swp1 -> VLAN 10"
        echo "swp2 -> VLAN 20"

        ;;


    *)

        echo
        echo "======================================"
        echo "Mini VLAN-Aware L2 Switch Control"
        echo "======================================"

        echo
        echo "Usage:"
        echo
        echo "  sudo ./switchctl.sh mac"
        echo "      Display VLAN-aware MAC address table"
        echo
        echo "  sudo ./switchctl.sh stats"
        echo "      Display switch statistics"
        echo
        echo "  sudo ./switchctl.sh vlan"
        echo "      Display VLAN configuration"
        echo
        echo "  sudo ./switchctl.sh clear"
        echo "      Clear MAC address table"
        echo

        ;;

esac