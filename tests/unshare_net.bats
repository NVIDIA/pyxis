#!/usr/bin/env bats

load ./common

function setup() {
    enroot_cleanup unshare-net unshare-nonet || true
}

function teardown() {
    enroot_cleanup unshare-net unshare-nonet || true
}

@test "--container-unshare=net creates new netns" {
    host_netns=$(readlink /proc/self/ns/net)
    run_srun --container-unshare=net --container-image=ubuntu:24.04 readlink /proc/self/ns/net
    [ "${lines[-1]}" != "${host_netns}" ]
}

@test "--container-unshare=net: multiple tasks" {
    host_netns=$(readlink /proc/self/ns/net)
    run_srun --ntasks=4 --container-unshare=net --container-image=ubuntu:24.04 \
        readlink /proc/self/ns/net

    netns_ids=$(grep -E '^net:\[[0-9]+\]$' <<< "${output}" | sort -u)
    [ "$(wc -l <<< "${netns_ids}")" -eq 1 ]
    [ "${netns_ids}" != "${host_netns}" ]
}

@test "--container-unshare=net loopback check" {
    run_srun_unchecked --container-unshare=net --container-image=ubuntu:24.04 \
        bash -c 'exec </dev/tcp/127.0.0.1/65535'
    [ "${status}" -ne 0 ]
    grep -q 'Connection refused' <<< "${output}"
}

@test "--container-unshare=net external network check" {
    run_srun_unchecked --container-unshare=net --container-image=ubuntu:24.04 \
        bash -c 'exec </dev/tcp/8.8.8.8/53'
    [ "${status}" -ne 0 ]
    grep -q 'Network is unreachable' <<< "${output}"
}

@test "no --container-unshare shares host netns" {
    host_netns=$(readlink /proc/self/ns/net)
    run_srun --container-image=ubuntu:24.04 readlink /proc/self/ns/net
    [ "${lines[-1]}" == "${host_netns}" ]
}

@test "--container-name joins existing netns container" {
    srun --overcommit --container-image=ubuntu:24.04 --container-name=unshare-net \
         --container-unshare=net sleep 30 &
    job_pid=$!
    sleep 5

    host_netns=$(readlink /proc/self/ns/net)
    run_srun --container-name=unshare-net readlink /proc/self/ns/net
    [ "${lines[-1]}" != "${host_netns}" ]

    wait ${job_pid} || true
}

@test "--container-unshare=net on existing container without netns" {
    srun --overcommit --container-image=ubuntu:24.04 --container-name=unshare-nonet \
         sleep 30 &
    job_pid=$!
    sleep 5

    run_srun_unchecked --container-name=unshare-nonet --container-unshare=net true
    [ "${status}" -ne 0 ]
    grep -q "not in a separate network namespace" <<< "${output}"

    wait ${job_pid} || true
}
