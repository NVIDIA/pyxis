#!/usr/bin/env bats

load ./common

function setup() {
    enroot_cleanup exec-test || true
}

function teardown() {
    enroot_cleanup exec-test || true
}

@test "enroot exec" {
    run_srun --container-image=ubuntu:18.04 --container-name=exec-test true
    run_srun --container-name=exec-test sleep 30s &

    sleep 5s # FIXME...
    run_enroot list -f
    pid=$(awk -vNAME1=pyxis_exec-test -vNAME2=pyxis_${SLURM_JOB_ID}_exec-test '($1 == NAME1 || $1 == NAME2) { print $2 }' <<< "${output}")
    logf "pid: %s" "${pid}"
    [ "${pid}" -gt "1" ]
    run_enroot exec "${pid}" true
}

@test "attach to running container" {
    run_srun --container-image=ubuntu:18.04 --container-name=exec-test mkdir /mymnt
    run_srun --container-name=exec-test --container-remap-root bash -c "mount -t tmpfs none /mymnt && sleep 30s" &

    sleep 5s # FIXME...
    run_srun --overlap --container-name=exec-test findmnt /mymnt
}

@test "attach to running unnamed container" {
    run_srun sh -c 'echo $SLURM_JOB_ID'
    job_id="${lines[-1]}"
    run_srun sh -c 'echo $SLURM_STEP_ID'
    step_id="${lines[-1]}"
    container_name="${job_id}.$((step_id + 1))"

    run_srun --container-image=ubuntu:22.04 sh -c 'sleep 10s' &

    sleep 3s
    run_srun --overlap --container-name="${container_name}" true
}

@test "attach to running container after directory change" {
    run_srun --container-image=ubuntu:20.04 --container-name=exec-test bash -c "cd /var && sleep 30s" &

    sleep 5s
    run_srun --overlap --container-name=exec-test pwd
    [ "${lines[-1]}" == "/" ]
}

@test "attach to running container with --container--mounts" {
    run_srun --container-image=ubuntu:20.04 --container-name=exec-test bash -c "sleep 30s" &

    sleep 5s
    # Verify that --container-mounts is ignored when attaching to a running container.
    run_srun --overlap --container-name=exec-test --container-mounts /tmp:/mnt/pyxis_test bash -c '[ ! -d "/mnt/pyxis_test" ]'
    grep -q -- 'ignoring --container-mounts' <<< "${output}"
}

@test "--container-name exec flag on stopped container" {
    run_srun --container-image=ubuntu:22.04 --container-name=exec-test sleep 1s

    run_srun_unchecked --container-name=exec-test:exec true
    [ "${status}" -ne 0 ]
}

@test "--container-name no_exec flag on running container" {
    run_srun --container-image=ubuntu:22.04 --container-name=exec-test mkdir /mymnt
    run_srun --container-name=exec-test --container-remap-root bash -c "mount -t tmpfs none /mymnt && sleep 30s" &

    sleep 5s
    run_srun --overlap --container-name=exec-test:no_exec bash -c '! findmnt /mymnt'
}
