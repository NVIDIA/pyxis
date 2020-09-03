#!/usr/bin/env bats

load ./common

function setup() {
    enroot remove -f pyxis_${SLURM_JOB_ID}_name-test >/dev/null 2>&1 || true
    enroot remove -f pyxis_${SLURM_JOB_ID}_name-test2 >/dev/null 2>&1 || true
}

function teardown() {
    enroot remove -f pyxis_${SLURM_JOB_ID}_name-test >/dev/null 2>&1 || true
    enroot remove -f pyxis_${SLURM_JOB_ID}_name-test2 >/dev/null 2>&1 || true
}

@test "unnamed container cleanup" {
    run_srun --container-image=ubuntu:18.04 sh -c 'echo $SLURM_JOB_ID.$SLURM_STEP_ID'
    container_name="${lines[-1]}"

    # Container removal is done async after the job is finished, poll for a while
    i=0
    while [ "$(enroot list | grep -c ${container_name})" -ne 0 ]; do
	((i++ == 100)) && exit 1
	sleep 0.1s
    done
}

@test "named container persistence" {
    run_srun --container-image=ubuntu:18.04 bash -c '! which file'
    run_srun --container-image=ubuntu:18.04 --container-name=name-test --container-remap-root bash -c 'apt-get update && apt-get install -y --no-install-recommends file'
    run_srun --container-image=ubuntu:18.04 --container-name=name-test which file
    run_srun --container-name=name-test which file

    run_srun --container-image=ubuntu:18.04 bash -c '! which file'
    run_srun --container-image=ubuntu:18.04 --container-name=name-test2 true
    run_srun --container-image=ubuntu:18.04 bash -c '! which file'
}

@test "named container manual remove" {
    run_srun --container-image=ubuntu:18.04 --container-name=name-test --container-remap-root bash -c 'apt-get update && apt-get install -y --no-install-recommends file'
    run_srun --container-name=name-test which file
    run_enroot remove -f pyxis_${SLURM_JOB_ID}_name-test

    run_srun_unchecked --container-name=name-test which file
    [ "${status}" -ne 0 ]
    run_srun --container-image=ubuntu:18.04 --container-name=name-test bash -c '! which file'
    run_srun --container-name=name-test bash -c '! which file'
}
