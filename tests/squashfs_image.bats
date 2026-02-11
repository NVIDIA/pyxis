#!/usr/bin/env bats

load ./common

function setup() {
    rm -f *.sqsh || true
    enroot_cleanup squashfs-test || true
}

function teardown() {
    rm -f *.sqsh || true
    enroot_cleanup squashfs-test || true
}

@test "Ubuntu 24.04 squashfs" {
    run_enroot import -o ubuntu.sqsh docker://ubuntu:24.04
    run_srun --container-image=./ubuntu.sqsh grep 'Ubuntu 24.04' /etc/os-release
}

@test "import - export - import" {
    run_enroot import -o ubuntu.sqsh docker://ubuntu:24.04
    run_srun --container-image=./ubuntu.sqsh --container-name=squashfs-test --container-remap-root sh -c 'apt-get update && apt-get install -y file'
    run_srun --container-name=squashfs-test sh -c 'echo pyxis > /test'
    run_enroot export -o ubuntu-modified.sqsh pyxis_squashfs-test || run_enroot export -o ubuntu-modified.sqsh pyxis_${SLURM_JOB_ID}_squashfs-test
    run_srun --container-image=./ubuntu-modified.sqsh which file
    run_srun --container-image=./ubuntu-modified.sqsh cat /test
    [ "${lines[-1]}" == "pyxis" ]
}

@test "--container-save absolute path" {
    readonly image="$(pwd)/ubuntu-modified.sqsh"
    run_srun --container-image=ubuntu:24.04 --container-save=${image} sh -c 'echo pyxis > /test' ; sleep 1s
    run_srun --container-image=${image} --container-save=${image} sh -c 'echo slurm > /test2' ; sleep 1s
    run_srun --container-image=${image} --container-save=${image} --container-remap-root sh -c 'apt-get update && apt-get install -y file' ; sleep 1s
    run_srun --container-image=${image} which file
    run_srun --container-image=${image} cat /test
    [ "${lines[-1]}" == "pyxis" ]
    run_srun --container-image=${image} cat /test2
    [ "${lines[-1]}" == "slurm" ]
}

@test "--container-save relative path" {
    readonly image="./ubuntu-modified.sqsh"
    run_srun --container-image=ubuntu:24.04 --container-save=${image} sh -c 'echo pyxis > /test' ; sleep 3s
    run_srun --container-image=${image} --container-save=${image} sh -c 'echo slurm > /test2' ; sleep 3s
    run_srun --container-image=${image} --container-save=${image} --container-remap-root sh -c 'apt-get update && apt-get install -y file' ; sleep 3s
    run_srun --container-image=${image} which file
    run_srun --container-image=${image} cat /test
    [ "${lines[-1]}" == "pyxis" ]
    run_srun --container-image=${image} cat /test2
    [ "${lines[-1]}" == "slurm" ]
}

@test "squashfs with --container-writable" {
    run_enroot import -o ubuntu.sqsh docker://ubuntu:24.04
    run_srun --container-image=./ubuntu.sqsh --container-writable bash -c 'echo test > /tmp/file.txt && cat /tmp/file.txt'
    [[ "${output}" =~ "test" ]]
}

@test "squashfs with --container-mounts" {
    run_enroot import -o ubuntu.sqsh docker://ubuntu:24.04
    echo "mounted content" > test-file.txt
    run_srun --container-image=./ubuntu.sqsh --container-mounts=$(pwd)/test-file.txt:/mnt/test.txt cat /mnt/test.txt
    [[ "${output}" =~ "mounted content" ]]
    rm -f test-file.txt
}

@test "squashfs with --container-workdir" {
    run_enroot import -o ubuntu.sqsh docker://ubuntu:24.04
    run_srun --container-image=./ubuntu.sqsh --container-workdir=/tmp pwd
    [ "${lines[-1]}" == "/tmp" ]
}

@test "squashfs with --no-container-remap-root" {
    run_enroot import -o ubuntu.sqsh docker://ubuntu:24.04
    run_srun --container-image=./ubuntu.sqsh --no-container-remap-root id -u
    [ "${lines[-1]}" -eq $(id -u) ]
}

@test "squashfs with multiple tasks" {
    run_enroot import -o ubuntu.sqsh docker://ubuntu:24.04
    run_srun -n 4 --container-image=./ubuntu.sqsh hostname
    [ "${status}" -eq 0 ]
    [ "${#lines[@]}" -ge 4 ]
}

@test "squashfs concurrent access" {
    run_enroot import -o ubuntu.sqsh docker://ubuntu:24.04
    srun --container-image=./ubuntu.sqsh sleep 2 &
    pid1=$!
    srun --container-image=./ubuntu.sqsh sleep 2 &
    pid2=$!
    wait ${pid1}; status1=$?
    wait ${pid2}; status2=$?
    [ "${status1}" -eq 0 ]
    [ "${status2}" -eq 0 ]
}

@test "squashfs container cleanup" {
    run_enroot import -o ubuntu.sqsh docker://ubuntu:24.04
    run_srun --container-image=./ubuntu.sqsh true
    run_srun --container-image=./ubuntu.sqsh true

    run_enroot list
    ! grep -q "pyxis_" <<< "${output}"
}

@test "squashfs nonexistent file" {
    run_srun_unchecked --container-image=./nonexistent.sqsh true
    [ "${status}" -ne 0 ]
}
