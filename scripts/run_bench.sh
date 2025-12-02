#!/bin/bash

source ./setup.sh

N=3
run_bench(){
    local NTHREADS=$1
    echo "NTHREADS = $NTHREADS"
    for i in $(seq 0 $(( N - 1 ))); do
        local M=${lines[$i]}
        run_cmd $M "sudo pkill bench; ~/atomic/tests/bench $i $NTHREADS" 2>"latency_${i}.csv" &
    done
    wait

    TMPFILE=$(mktemp)
    OUTFILE="latency_${N}_${NTHREADS}.csv"
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
