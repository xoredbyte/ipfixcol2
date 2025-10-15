#!/bin/bash

DEST="localhost"
PORT=4739
PROTO="UDP"
INPUT="sample.ipfix"
N=""          # empty means infinite sends
SPEED="1G"    # Default speed 1Gbps
CORES=4       # Number of cores to use

# Base ODID
BASE_ODID=57786

# Store PIDs of all running senders
declare -a SENDER_PIDS=()

# Function to start ipfixsend2 instances
start_senders() {
    echo "Starting $CORES ipfixsend2 processes with speed=$SPEED"
    SENDER_PIDS=()  # reset array
    for ((core=0; core<CORES; core++)); do
        ODID=$((BASE_ODID + core))
        echo "Launching core $core with ODID=$ODID"
        
        CMD="taskset -c $core ipfixsend2 -d $DEST -p $PORT -t $PROTO -i $INPUT -s $SPEED -O $ODID"
        [[ -n "$N" ]] && CMD="$CMD -n $N"
        
        eval "$CMD &"
        SENDER_PIDS+=($!)
    done
}

# Function to stop all running senders
stop_senders() {
    for pid in "${SENDER_PIDS[@]}"; do
        kill "$pid" 2>/dev/null
    done
    SENDER_PIDS=()
}

# Start the first run
start_senders

# Loop for user input to adjust cores or speed dynamically
while true; do
    read -p "Enter new core count (C=<num>), new speed (S=<value>, e.g., 500M), or 'q' to quit: " INPUT_VAL
    if [[ "$INPUT_VAL" == "q" ]]; then
        echo "Stopping..."
        stop_senders
        break
    elif [[ "$INPUT_VAL" =~ ^C=([0-9]+)$ ]]; then
        CORES="${BASH_REMATCH[1]}"
        echo "Updating core count to $CORES..."
        stop_senders
        start_senders
    elif [[ "$INPUT_VAL" =~ ^S=([0-9]+[KMG]?)$ ]]; then
        SPEED="${BASH_REMATCH[1]}"
        echo "Updating speed to $SPEED..."
        stop_senders
        start_senders
    else
        echo "Invalid input. Use 'C=<cores>' or 'S=<speed>' (e.g., 500M), or 'q' to quit."
    fi
done
