#!/usr/bin/env bats

load ./common

function setup() {
    enroot_cleanup unshare-ipc unshare-noipc || true
}

function teardown() {
    enroot_cleanup unshare-ipc unshare-noipc || true
}

@test "--container-unshare=ipc creates new ipcns" {
    host_ipcns=$(readlink /proc/self/ns/ipc)
    run_srun --container-unshare=ipc --container-image=ubuntu:24.04 readlink /proc/self/ns/ipc
    [ "${lines[-1]}" != "${host_ipcns}" ]
}

@test "--container-unshare=ipc: multiple tasks" {
    host_ipcns=$(readlink /proc/self/ns/ipc)
    run_srun --ntasks=4 --container-unshare=ipc --container-image=ubuntu:24.04 \
        readlink /proc/self/ns/ipc

    ipcns_ids=$(grep -E '^ipc:\[[0-9]+\]$' <<< "${output}" | sort -u)
    [ "$(wc -l <<< "${ipcns_ids}")" -eq 1 ]
    [ "${ipcns_ids}" != "${host_ipcns}" ]
}

@test "--container-unshare=ipc isolates SysV shared memory" {
    # A SysV shm segment created inside the container's IPC namespace
    # must not be visible on the host.
    run_srun --container-unshare=ipc --container-image=ubuntu:24.04 \
        bash -c 'ipcmk -M 1M >/dev/null && ipcs -m | grep -c "^0x"'
    [ "${lines[-1]}" -ge 1 ]

    # After the container exits, its IPC namespace goes away and all its
    # segments are freed by the kernel. Host ipcs -m must show no trace.
    before=$(ipcs -m)
    run_srun --container-unshare=ipc --container-image=ubuntu:24.04 \
        bash -c 'ipcmk -M 1M >/dev/null'
    after=$(ipcs -m)
    [ "${before}" == "${after}" ]
}

@test "--container-unshare=ipc uses private /dev, /dev/shm and /dev/mqueue" {
    host_dev_dev="$(stat -c %d /dev)"
    host_shm_dev="$(stat -c %d /dev/shm)"
    host_mqueue_dev="$(stat -c %d /dev/mqueue)"

    ENROOT_RESTRICT_DEV=no HOST_DEV_DEV="${host_dev_dev}" HOST_SHM_DEV="${host_shm_dev}" HOST_MQUEUE_DEV="${host_mqueue_dev}" run_srun \
        --container-unshare=ipc \
        --container-env=HOST_DEV_DEV,HOST_SHM_DEV,HOST_MQUEUE_DEV \
        --container-image=ubuntu:24.04 \
        bash -c '[ "$(stat -c %d /dev)" != "${HOST_DEV_DEV}" ] && [ "$(stat -c %d /dev/shm)" != "${HOST_SHM_DEV}" ] && [ "$(stat -c %d /dev/mqueue)" != "${HOST_MQUEUE_DEV}" ]'
}

@test "no --container-unshare shares host ipcns" {
    host_ipcns=$(readlink /proc/self/ns/ipc)
    run_srun --container-image=ubuntu:24.04 readlink /proc/self/ns/ipc
    [ "${lines[-1]}" == "${host_ipcns}" ]
}

@test "--container-name joins existing ipcns container" {
    srun --overcommit --container-image=ubuntu:24.04 --container-name=unshare-ipc \
         --container-unshare=ipc sleep 30 &
    job_pid=$!
    sleep 5

    host_ipcns=$(readlink /proc/self/ns/ipc)
    run_srun --container-name=unshare-ipc readlink /proc/self/ns/ipc
    [ "${lines[-1]}" != "${host_ipcns}" ]

    wait ${job_pid} || true
}

@test "--container-unshare=ipc on existing container without ipcns" {
    srun --overcommit --container-image=ubuntu:24.04 --container-name=unshare-noipc \
         sleep 30 &
    job_pid=$!
    sleep 5

    run_srun_unchecked --container-name=unshare-noipc --container-unshare=ipc true
    [ "${status}" -ne 0 ]
    grep -q "not in a separate IPC namespace" <<< "${output}"

    wait ${job_pid} || true
}
