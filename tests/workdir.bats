#!/usr/bin/env bats

load ./common

@test "--container-workdir absolute target" {
    run_srun_unchecked --container-workdir=/usr/local/bin --container-image=ubuntu:18.04 pwd
    [ "${status}" -eq 0 ]
    [ "${lines[-1]}" == "/usr/local/bin" ]
}

@test "--container-workdir path escape attempt" {
    run_srun_unchecked --container-workdir=../ --container-image=ubuntu:18.04 pwd
    [ "${status}" -eq 0 ]
    [ "${lines[-1]}" == "/" ]
}

@test "--container-workdir target doesn't exist" {
    run_srun_unchecked --container-workdir=/pyxis --container-image=ubuntu:18.04 true
    [ "${status}" -ne 0 ]
    grep -q '/pyxis: No such file or directory' <<< "${output}"
}

@test "container image workdir" {
    run_srun_unchecked --container-image=nvcr.io/nvidia/pytorch:20.02-py3 pwd
    [ "${status}" -eq 0 ]
    [ "${lines[-1]}" == "/workspace" ]
}
