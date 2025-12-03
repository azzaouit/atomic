#!/bin/bash

source ./setup.sh

N=7
SLOTS=1000000
run_bench(){
    local TOTAL_THREADS=$1
    local BASE_THREADS=$((TOTAL_THREADS / N))
    local EXTRA_THREADS=$((TOTAL_THREADS % N))
    local REQ_PER_THREAD=$((SLOTS/TOTAL_THREADS))
    echo "TOTAL_THREADS = ${TOTAL_THREADS}, REQ_PER_THREAD = ${REQ_PER_THREAD}"
    for i in $(seq 0 $(( N - 1 ))); do
        local M=${lines[$i]}
        if [ $i -lt $EXTRA_THREADS ]; then
            local NODE_THREADS=$((BASE_THREADS + 1))
        else
            local NODE_THREADS=$BASE_THREADS
        fi
        echo "  Node $i: $NODE_THREADS threads"
        run_cmd $M "sudo pkill bench; ~/atomic/tests/bench $i $NODE_THREADS $REQ_PER_THREAD" 2>"latency_${i}.csv" &
    done
    wait
    TMPFILE=$(mktemp)
    OUTFILE="latency_${N}_${TOTAL_THREADS}.csv"
    header="Host ID,Thread ID,Slot,Latency"
    cat latency_*.csv | grep "^[0-9]" > $TMPFILE
    rm latency_*.csv
    echo $header > $OUTFILE
    sort -t, -k3,3 -n $TMPFILE >> $OUTFILE
    rm $TMPFILE
    mv $OUTFILE data
}

build_all

run_bench 1
run_bench 10
run_bench 100
run_bench 1000
