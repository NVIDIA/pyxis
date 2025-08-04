#!/usr/bin/env bats

load ./common

function setup() {
    enroot_cleanup env-test env-test2 || true
}

function teardown() {
    enroot_cleanup env-test env-test2 || true
}

@test "PYXIS_CONTAINER_IMAGE environment variable" {
    PYXIS_CONTAINER_IMAGE=ubuntu:22.04 run_srun grep 'Ubuntu 22.04' /etc/os-release
}

@test "PYXIS_CONTAINER_IMAGE: command line precedence" {
    PYXIS_CONTAINER_IMAGE=ubuntu:22.04 run_srun --container-image=ubuntu:24.04 grep 'Ubuntu 24.04' /etc/os-release
}

@test "PYXIS_CONTAINER_MOUNTS environment variable" {
    PYXIS_CONTAINER_MOUNTS=/home:/test-mnt run_srun --container-image=ubuntu:24.04 findmnt /test-mnt
}

@test "PYXIS_CONTAINER_MOUNTS: command line precedence" {
    PYXIS_CONTAINER_MOUNTS=/home:/mnt-env run_srun --container-image=ubuntu:24.04 --container-mounts=/tmp:/mnt-cli bash -c 'findmnt /mnt-cli && ! findmnt /mnt-env'
}

@test "PYXIS_CONTAINER_WORKDIR environment variable" {
    PYXIS_CONTAINER_WORKDIR=/usr/local/bin run_srun --container-image=ubuntu:24.04 pwd
    [ "${lines[-1]}" == "/usr/local/bin" ]
}

@test "PYXIS_CONTAINER_WORKDIR: command line precedence" {
    PYXIS_CONTAINER_WORKDIR=/usr/local/bin run_srun --container-image=ubuntu:24.04 --container-workdir=/tmp pwd
    [ "${lines[-1]}" == "/tmp" ]
}

@test "PYXIS_CONTAINER_NAME environment variable" {
    PYXIS_CONTAINER_NAME=env-test run_srun --container-image=ubuntu:24.04 --container-remap-root bash -c 'apt-get update && apt-get install -y --no-install-recommends file'
    run_srun --container-name=env-test which file
}

@test "PYXIS_CONTAINER_NAME: command line precedence" {
    PYXIS_CONTAINER_NAME=env-test run_srun --container-image=ubuntu:24.04 --container-name=env-test2 --container-remap-root bash -c 'apt-get update && apt-get install -y --no-install-recommends make'
    run_srun --container-name=env-test2 which make
    run_srun_unchecked --container-name=env-test which make
    [ "${status}" -ne 0 ]
}

@test "PYXIS_CONTAINER_SAVE environment variable" {
    readonly image="./env-test.sqsh"
    rm -f "${image}"
    PYXIS_CONTAINER_SAVE=${image} run_srun --container-image=ubuntu:24.04 sh -c 'echo pyxis > /test'
    sleep 1s
    run_srun --container-image=${image} cat /test
    [ "${lines[-1]}" == "pyxis" ]
    rm -f "${image}"
}

@test "PYXIS_CONTAINER_SAVE: command line precedence" {
    readonly env_image="./env-test.sqsh"
    readonly cli_image="./cli-test.sqsh"
    rm -f "${env_image}" "${cli_image}"
    PYXIS_CONTAINER_SAVE=${env_image} run_srun --container-image=ubuntu:24.04 --container-save=${cli_image} sh -c 'echo pyxis > /test'
    sleep 1s
    run_srun --container-image=${cli_image} cat /test
    [ "${lines[-1]}" == "pyxis" ]
    [ ! -f "${env_image}" ]
    rm -f "${cli_image}"
}

@test "PYXIS_CONTAINER_MOUNT_HOME=1 environment variable" {
    PYXIS_CONTAINER_MOUNT_HOME=1 run_srun --container-remap-root --container-image=ubuntu:24.04 findmnt /root
}

@test "PYXIS_CONTAINER_MOUNT_HOME=0 environment variable" {
    PYXIS_CONTAINER_MOUNT_HOME=0 run_srun --container-remap-root --container-image=ubuntu:24.04 bash -c '! findmnt /root'
}

@test "PYXIS_CONTAINER_MOUNT_HOME: command line precedence" {
    PYXIS_CONTAINER_MOUNT_HOME=1 run_srun --no-container-mount-home --container-remap-root --container-image=ubuntu:24.04 bash -c '! findmnt /root'
}

@test "PYXIS_CONTAINER_REMAP_ROOT=1 environment variable" {
    PYXIS_CONTAINER_REMAP_ROOT=1 run_srun --container-image=ubuntu:24.04 bash -c 'cat /proc/self/uid_map'
    uidmap=(${lines[-1]})
    [ "${uidmap[0]}" -eq 0 ]
    [ "${uidmap[1]}" -eq $(id -u) ]
    [ "${uidmap[2]}" -eq 1 ]
}

@test "PYXIS_CONTAINER_REMAP_ROOT=0 environment variable" {
    PYXIS_CONTAINER_REMAP_ROOT=0 run_srun --container-image=ubuntu:24.04 bash -c 'cat /proc/self/uid_map'
    uidmap=(${lines[-1]})
    [ "${uidmap[0]}" -eq $(id -u) ]
    [ "${uidmap[1]}" -eq $(id -u) ]
    [ "${uidmap[2]}" -eq 1 ]
}

@test "PYXIS_CONTAINER_REMAP_ROOT: command line precedence" {
    PYXIS_CONTAINER_REMAP_ROOT=1 run_srun --no-container-remap-root --container-image=ubuntu:24.04 bash -c 'cat /proc/self/uid_map'
    uidmap=(${lines[-1]})
    [ "${uidmap[0]}" -eq $(id -u) ]
    [ "${uidmap[1]}" -eq $(id -u) ]
    [ "${uidmap[2]}" -eq 1 ]
}

@test "PYXIS_CONTAINER_ENTRYPOINT=1 environment variable" {
    if srun bash -c '[ -f /etc/enroot/entrypoint ]'; then
        skip "entrypoint disabled by enroot"
    fi

    PYXIS_CONTAINER_ENTRYPOINT=1 run_srun --container-image=docker:26.1.0-dind-rootless sh -c '[ -n "${DOCKER_HOST}" ]'
}

@test "PYXIS_CONTAINER_ENTRYPOINT=0 environment variable" {
    PYXIS_CONTAINER_ENTRYPOINT=0 run_srun --container-image=docker:26.1.0-dind-rootless sh -c '[ -z "${DOCKER_HOST}" ]'
}

@test "PYXIS_CONTAINER_ENTRYPOINT: command line precedence" {
    if srun bash -c '[ -f /etc/enroot/entrypoint ]'; then
        skip "entrypoint disabled by enroot"
    fi

    PYXIS_CONTAINER_ENTRYPOINT=0 run_srun --container-entrypoint --container-image=docker:26.1.0-dind-rootless sh -c '[ -n "${DOCKER_HOST}" ]'
}

@test "PYXIS_CONTAINER_ENTRYPOINT_LOG=1 environment variable" {
    if srun bash -c '[ -f /etc/enroot/entrypoint ]'; then
        skip "entrypoint disabled by enroot"
    fi

    PYXIS_CONTAINER_ENTRYPOINT_LOG=1 run_srun --container-entrypoint --container-image=nvidia/cuda:12.9.1-runtime-ubuntu24.04 true
    grep -q "== CUDA ==" <<< "${output}"
}

@test "PYXIS_CONTAINER_ENTRYPOINT_LOG=0 environment variable" {
    if srun bash -c '[ -f /etc/enroot/entrypoint ]'; then
        skip "entrypoint disabled by enroot"
    fi

    PYXIS_CONTAINER_ENTRYPOINT_LOG=0 run_srun --container-entrypoint --container-image=nvidia/cuda:12.9.1-runtime-ubuntu24.04 true
    ! grep -q "== CUDA ==" <<< "${output}"
}

@test "PYXIS_CONTAINER_WRITABLE=1 environment variable" {
    PYXIS_CONTAINER_WRITABLE=1 run_srun --container-image=ubuntu:24.04 touch /newfile
}

@test "PYXIS_CONTAINER_WRITABLE=0 environment variable" {
    PYXIS_CONTAINER_WRITABLE=0 run_srun_unchecked --container-image=ubuntu:24.04 touch /newfile
    [ "${status}" -ne 0 ]
    grep -q 'Read-only file system' <<< "${output}"
}

@test "PYXIS_CONTAINER_WRITABLE: command line precedence" {
    PYXIS_CONTAINER_WRITABLE=1 run_srun_unchecked --container-readonly --container-image=ubuntu:24.04 touch /newfile
    [ "${status}" -ne 0 ]
    grep -q 'Read-only file system' <<< "${output}"
}

@test "PYXIS_CONTAINER_READONLY=1 environment variable" {
    PYXIS_CONTAINER_READONLY=1 run_srun_unchecked --container-image=ubuntu:24.04 touch /newfile
    [ "${status}" -ne 0 ]
    grep -q 'Read-only file system' <<< "${output}"
}

@test "PYXIS_CONTAINER_READONLY=0 environment variable" {
    PYXIS_CONTAINER_READONLY=0 run_srun_unchecked --container-image=ubuntu:24.04 touch /newfile
}

@test "PYXIS_CONTAINER_READONLY: command line precedence" {
    PYXIS_CONTAINER_READONLY=0 run_srun_unchecked --container-readonly --container-image=ubuntu:24.04 touch /newfile
    [ "${status}" -ne 0 ]
    grep -q 'Read-only file system' <<< "${output}"
}

@test "PYXIS_CONTAINER_ENV environment variable" {
    export CUDA_VERSION=11.0.0
    PYXIS_CONTAINER_ENV=CUDA_VERSION run_srun --no-container-mount-home --container-image=nvidia/cuda:12.9.1-base-ubuntu24.04 sh -c 'echo $CUDA_VERSION'
    [ "${lines[-1]}" == "11.0.0" ]
}

@test "PYXIS_CONTAINER_ENV: command line precedence" {
    export CUDA_VERSION=12.0.0
    export LD_LIBRARY_PATH=/usr/local/cuda/lib64
    PYXIS_CONTAINER_ENV=CUDA_VERSION run_srun --no-container-mount-home --container-image=nvidia/cuda:12.9.1-base-ubuntu24.04 --container-env=LD_LIBRARY_PATH sh -c 'echo $CUDA_VERSION $LD_LIBRARY_PATH'
    [[ "${lines[-1]}" == "12.9.1 /usr/local/cuda/lib64" ]]
}
