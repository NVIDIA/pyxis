#!/usr/bin/env bats

load ./common

function setup() {
    enroot remove -f pyxis_${SLURM_JOB_ID}_exec-test || true
}

function teardown() {
    enroot remove -f pyxis_${SLURM_JOB_ID}_exec-test || true
}

@test "enroot exec" {
    run_srun --container-image=ubuntu:18.04 --container-name=exec-test true
    run_srun --container-name=exec-test sleep 30s &

    sleep 5s # FIXME...
    pid=$(enroot list -f | awk -vNAME=pyxis_${SLURM_JOB_ID}_exec-test '($1 == NAME) { print $2 }')
    logf "pid: %s" "${pid}"
    [ "${pid}" -gt "1" ]
    run_enroot exec "${pid}" true
}

@test "attach to running container" {
    run_srun --container-image=ubuntu:18.04 --container-name=exec-test mkdir /mymnt
    run_srun --container-name=exec-test --container-remap-root bash -c "mount -t tmpfs none /mymnt && sleep 30s" &

    sleep 5s # FIXME...
    run_srun --container-name=exec-test findmnt /mymnt
}
