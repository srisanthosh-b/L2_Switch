#!/bin/bash

PROC_DIR="/proc/l2switch"

case "$1" in

    mac)

        echo "===== MAC ADDRESS TABLE ====="

        if [ ! -f "$PROC_DIR/mac" ]; then
            echo "Error: l2switch module is not loaded."
            exit 1
        fi

        cat "$PROC_DIR/mac"

        ;;


    stats)

        echo "===== SWITCH STATISTICS ====="

        if [ ! -f "$PROC_DIR/stats" ]; then
            echo "Error: l2switch module is not loaded."
            exit 1
        fi

        cat "$PROC_DIR/stats"

        ;;


    clear)

        echo "MAC table clearing is not implemented yet."

        ;;


    *)

        echo
        echo "Mini L2 Switch Control"
        echo
        echo "Usage:"
        echo
        echo "  sudo ./switchctl.sh mac"
        echo "      Display MAC address table"
        echo
        echo "  sudo ./switchctl.sh stats"
        echo "      Display switch statistics"
        echo
        echo "  sudo ./switchctl.sh clear"
        echo "      Clear MAC table (not implemented)"
        echo

        ;;

esac
