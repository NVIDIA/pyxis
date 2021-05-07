#!/usr/bin/env bats

load ./common

function setup() {
    enroot remove -f pyxis_sshd pyxis_${SLURM_JOB_ID}_sshd >/dev/null 2>&1 || true
}

function teardown() {
    enroot remove -f pyxis_sshd pyxis_${SLURM_JOB_ID}_sshd >/dev/null 2>&1 || true
}

# From https://github.com/NVIDIA/pyxis/issues/44
@test "ssh to container running sshd" {
    run_srun --container-image ubuntu:20.04 --container-name=sshd --container-remap-root bash -c 'apt-get update && apt-get install -y --no-install-recommends openssh-server'
    run_srun --container-name=sshd --no-container-remap-root bash -c 'timeout --preserve-status --signal=TERM 10 /usr/sbin/sshd -d -p 2222' &

    sleep 3s
    # Force pseudo-terminal allocation with -t
    run ssh -t -p 2222 -o StrictHostKeyChecking=no localhost true
    [ "${status}" -eq 0 ]

    wait
}
