#!/usr/bin/env bats

load ./common

@test "--container-remap-root uid map" {
    run_srun --container-remap-root --container-image=ubuntu:18.04 bash -c 'cat /proc/self/uid_map'
    uidmap=(${lines[-1]})
    [ "${uidmap[0]}" -eq 0 ]
    [ "${uidmap[1]}" -eq $(id -u) ]
    [ "${uidmap[2]}" -eq 1 ]
}

@test "--no-container-remap-root uid map" {
    run_srun --no-container-remap-root --container-image=ubuntu:18.04 bash -c 'cat /proc/self/uid_map'
    uidmap=(${lines[-1]})
    [ "${uidmap[0]}" -eq $(id -u) ]
    [ "${uidmap[1]}" -eq $(id -u) ]
    [ "${uidmap[2]}" -eq 1 ]
}

@test "--container-remap-root seccomp" {
    run_srun --container-remap-root --container-image=ubuntu:18.04 grep 'Seccomp:[[:space:]]\+2' /proc/self/status
}

@test "--no-container-remap-root seccomp" {
    run_srun --no-container-remap-root --container-image=ubuntu:18.04 grep 'Seccomp:[[:space:]]\+2' /proc/self/status
}

@test "apt-get install file" {
    run_srun --container-remap-root --container-image=ubuntu:18.04 bash -c 'apt-get update && apt-get install -y --no-install-recommends file'
}

@test "apt-get install emacs-nox" {
    run_srun --container-remap-root --container-image=ubuntu:18.04 bash -c 'apt-get update && apt-get install -y --no-install-recommends emacs-nox'
}

@test "yum install file" {
    run_srun --container-remap-root --container-image=centos:7 bash -c 'yum install -y file'
}

@test "yum install emacs-nox" {
    run_srun --container-remap-root --container-image=centos:7 bash -c 'yum install -y emacs-nox'
}
