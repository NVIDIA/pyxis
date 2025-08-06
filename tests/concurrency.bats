#!/usr/bin/env bats

load ./common

function setup() {
    enroot_cleanup concurrent-test-{0..4} || true
}

function teardown() {
    enroot_cleanup concurrent-test-{0..4} || true
}

@test "concurrent container creation" {
    pids=()
    for i in {0..4}; do
        srun -n1 --overlap --container-image=ubuntu:24.04 --container-name=concurrent-test-$i sleep 5s &
        pids+=($!)
    done

    for pid in "${pids[@]}"; do
        wait $pid
        [ $? -eq 0 ]
    done
}

# Test for https://github.com/NVIDIA/enroot/issues/126
@test "concurrent container reuse" {
    for i in {0..4}; do
        run_srun --container-image=ubuntu:24.04 --container-name=concurrent-test-$i true
    done

    pids=()
    for i in {1..50}; do
        srun -n1 --overlap --container-name=concurrent-test-$((i % 5)):no_exec sleep 1 &
        pids+=($!)
        sleep "$(printf "0.%ds" $RANDOM)"
    done

    for pid in "${pids[@]}"; do
        wait $pid
        [ $? -eq 0 ]
    done
}
