#!/bin/bash

DEST="localhost"
PORT=4739
PROTO="UDP"
INPUT="sample.ipfix"
N=1000000000

# Initial R value
R=100000.0

# Number of cores to use (user configurable)
CORES=4

# Store PIDs of all running senders
declare -a SENDER_PIDS=()

# Function to start ipfixsend2 instances
start_senders() {
    echo "Starting $CORES ipfixsend2 processes with R=$R"
    SENDER_PIDS=()  # reset array
    for ((core=0; core<CORES; core++)); do
        # Pin each process to a specific CPU core
        taskset -c "$core" ipfixsend2 -d "$DEST" -p "$PORT" -t "$PROTO" \
            -i "$INPUT" -n "$N" -R "$R" &
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

# Loop for user input
while true; do
    read -p "Enter new R value or new core count (e.g. R=2000 or C=8, 'q' to quit): " INPUT_VAL
    if [[ "$INPUT_VAL" == "q" ]]; then
        echo "Stopping..."
        stop_senders
        break
    elif [[ "$INPUT_VAL" =~ ^R=([0-9]+(\.[0-9]+)?)$ ]]; then
        R="${BASH_REMATCH[1]}"
        echo "Updating R to $R..."
        stop_senders
        start_senders
    elif [[ "$INPUT_VAL" =~ ^C=([0-9]+)$ ]]; then
        CORES="${BASH_REMATCH[1]}"
        echo "Updating core count to $CORES..."
        stop_senders
        start_senders
    else
        echo "Invalid input. Use 'R=<rate>' or 'C=<cores>' or 'q' to quit."
    fi
done

