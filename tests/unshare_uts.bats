#!/usr/bin/env bats

load ./common

function setup() {
    enroot_cleanup unshare-uts unshare-nouts || true
}

function teardown() {
    enroot_cleanup unshare-uts unshare-nouts || true
}

@test "--container-unshare=uts creates new utsns" {
    host_utsns=$(readlink /proc/self/ns/uts)
    run_srun --container-unshare=uts --container-image=ubuntu:24.04 readlink /proc/self/ns/uts
    [ "${lines[-1]}" != "${host_utsns}" ]
}

@test "--container-unshare=uts: multiple tasks" {
    host_utsns=$(readlink /proc/self/ns/uts)
    run_srun --ntasks=4 --container-unshare=uts --container-image=ubuntu:24.04 \
        readlink /proc/self/ns/uts

    utsns_ids=$(grep -E '^uts:\[[0-9]+\]$' <<< "${output}" | sort -u)
    [ "$(wc -l <<< "${utsns_ids}")" -eq 1 ]
    [ "${utsns_ids}" != "${host_utsns}" ]
}

@test "--container-unshare=uts isolates sethostname" {
    host_hostname=$(hostname)
    run_srun --container-unshare=uts --container-image=ubuntu:24.04 hostname
    [ "${lines[-1]}" == "${host_hostname}" ]

    run_srun --container-unshare=uts --container-image=ubuntu:24.04 \
        bash -c 'hostname container-test && hostname'
    [ "${lines[-1]}" == "container-test" ]

    [ "$(hostname)" == "${host_hostname}" ]
}

@test "no --container-unshare shares host utsns" {
    host_utsns=$(readlink /proc/self/ns/uts)
    run_srun --container-image=ubuntu:24.04 readlink /proc/self/ns/uts
    [ "${lines[-1]}" == "${host_utsns}" ]
}

@test "--container-name joins existing utsns container" {
    srun --overcommit --container-image=ubuntu:24.04 --container-name=unshare-uts \
         --container-unshare=uts sleep 30 &
    job_pid=$!
    sleep 5

    host_utsns=$(readlink /proc/self/ns/uts)
    run_srun --container-name=unshare-uts readlink /proc/self/ns/uts
    [ "${lines[-1]}" != "${host_utsns}" ]

    wait ${job_pid} || true
}

@test "--container-unshare=uts on existing container without utsns" {
    srun --overcommit --container-image=ubuntu:24.04 --container-name=unshare-nouts \
         sleep 30 &
    job_pid=$!
    sleep 5

    run_srun_unchecked --container-name=unshare-nouts --container-unshare=uts true
    [ "${status}" -ne 0 ]
    grep -q "not in a separate UTS namespace" <<< "${output}"

    wait ${job_pid} || true
}
