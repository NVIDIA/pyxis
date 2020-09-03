#!/usr/bin/env bats

load ./common

@test "invalid arg: --container-image= (without argument)" {
    run_srun_unchecked --container-image= true
    [ "${status}" -ne 0 ]
}

@test "invalid arg: --container-image=<very long invalid image name>" {
    image="$(head -c 2048 /dev/urandom | base64)"
    run_srun_unchecked --container-image="${image}" true
    [ "${status}" -ne 0 ]
}

@test "invalid arg: --container-image=./" {
    run_srun_unchecked --container-image=./ true
    [ "${status}" -ne 0 ]
}

@test "invalid arg: --container-image=/dev/urandom" {
    run_srun_unchecked --container-image=/dev/urandom true
    [ "${status}" -ne 0 ]
}

@test "invalid arg: --container-name without --container-image" {
    run_srun_unchecked --container-name=args-test true
    [ "${status}" -ne 0 ]
}

@test "invalid arg: --container-name=/tmp/foo" {
    run_srun_unchecked --container-name=/tmp/foo --container-image=ubuntu:18.04 true
    [ "${status}" -ne 0 ]
}

@test "invalid arg: --container-mounts= (without argument)" {
    run_srun_unchecked --container-mounts=  --container-image=ubuntu:18.04 findmnt /foo
    [ "${status}" -ne 0 ]
}

@test "invalid arg: --container-mounts=:" {
    run_srun_unchecked --container-mounts=: --container-image=ubuntu:18.04 true
    [ "${status}" -ne 0 ]
}

@test "invalid arg: --container-mounts=/tmp:" {
    run_srun_unchecked --container-mounts=/tmp: --container-image=ubuntu:18.04 true
    [ "${status}" -ne 0 ]
}

@test "invalid arg: --container-workdir= (without argument)" {
    run_srun_unchecked --container-workdir= --container-image=ubuntu:18.04 true
    [ "${status}" -ne 0 ]
}
