#!/usr/bin/env bats

load ./common

function setup() {
    enroot remove -f pyxis-exec-test || true
}

function teardown() {
    enroot remove -f pyxis-exec-test || true
}


@test "enroot exec" {
    run_srun --container-image=ubuntu:18.04 --container-name=pyxis-exec-test true
    run_srun --container-name=pyxis-exec-test sleep 30s &

    sleep 5s # FIXME...
    pid=$(enroot list -f | awk -vNAME=pyxis-exec-test '($1 == NAME) { print $2 }')
    logf "pid: %s" "${pid}"
    [ "${pid}" -gt "1" ]
    run_enroot exec "${pid}" true
}
