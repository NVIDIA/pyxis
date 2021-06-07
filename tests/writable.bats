#!/usr/bin/env bats

load ./common

@test "--container-readonly" {
    run_srun_unchecked --container-readonly --container-image=ubuntu:20.04 touch /newfile
    [ "${status}" -ne 0 ]
    grep -q 'Read-only file system' <<< "${output}"
}

@test "--container-writable" {
    run_srun --container-writable --container-image=ubuntu:20.04 touch /newfile
}
