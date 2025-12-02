#!/bin/bash

# Cluster file: single node address on each line
CLUSTER_FILE="cluster.txt"
MACHS=$(cat $CLUSTER_FILE)
mapfile -t lines < $CLUSTER_FILE
NODES=${#lines[@]}
N=$(($NODES-1))
LEADER=${lines[0]}

# Common commands
UPDATE_CMD="sudo apt update -y && sudo apt upgrade -y"
DEPS_CMD="sudo apt install -y build-essential make rdma-core libibverbs-dev librdmacm-dev ibverbs-utils"
REBOOT_CMD="sudo reboot"
SRC_DIR=$(realpath ../atomic)

# Run a remote command over SSH
run_cmd() {
    local M=$1
    local CMD="${2}"
    ssh $M $CMD
}

# Wait for a host to be up after a reboot
wait_for_host() {
    HOST=$1
	PORT=22
	TIMEOUT=5
	echo "Waiting for SSH on $HOST:$PORT to be available..."
	while ! nc -z -w $TIMEOUT $HOST $PORT; do
	  echo "$HOST:$PORT down. Retrying..."
	  sleep $TIMEOUT
	done
    echo "$HOST:$PORT is up."
}

# Reboot the cluster and wait for the last host to be up
reboot_all_and_wait() {
    for i in $(seq 0 $N); do
          M=${lines[$i]}
          run_cmd $M "${REBOOT_CMD}"
    done
    wait_for_host ${lines[$N]}
}

# Setup dependencies on a fresh machine
setup() {
    local M=$1
    run_cmd $M "${UPDATE_CMD}"
    run_cmd $M "${DEPS_CMD}"
}

# Build libatomic on a single replica
build() {
    local M=$1
    run_cmd $M "rm -rf ~/atomic"
    rsync -avz $(realpath ../atomic) $M:~
    run_cmd $M "make -C ~/atomic clean all"
}

# Setup dependencies on every machine
setup_all() {
    for i in $(seq 0 $N); do
          local M=${lines[$i]}
          setup $M &
    done
    wait
}

# Build libatomic on every machine
build_all() {
    make -C $SRC_DIR clean
    for i in $(seq 0 $N); do
        M=${lines[$i]}
        build $M &
    done
    wait
}
