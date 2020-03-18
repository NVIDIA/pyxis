#!/usr/bin/env bats

load ./common

function setup() {
    rm -f *.sqsh || true
    enroot remove -f pyxis-squashfs-test || true
}

function teardown() {
    rm -f *.sqsh || true
    enroot remove -f pyxis-squashfs-test || true
}

@test "Ubuntu 18.04 squashfs" {
    run_enroot import -o ubuntu.sqsh docker://ubuntu:18.04
    run_srun --container-image=./ubuntu.sqsh grep 'Ubuntu 18.04' /etc/os-release
}

@test "import - export - import" {
    run_enroot import -o ubuntu.sqsh docker://ubuntu:18.04
    run_srun --container-image=./ubuntu.sqsh --container-name=pyxis-squashfs-test sh -c 'apt-get update && apt-get install -y file'
    run_srun --container-name=pyxis-squashfs-test sh -c 'echo pyxis > /test'
    run_enroot export -o ubuntu-modified.sqsh pyxis-squashfs-test
    run_srun --container-image=./ubuntu-modified.sqsh which file
    run_srun --container-image=./ubuntu-modified.sqsh cat /test
    [ "${lines[-1]}" == "pyxis" ]
}
