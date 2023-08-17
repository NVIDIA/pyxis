#!/usr/bin/env bats

load ./common

function setup() {
    enroot_cleanup sshd || true
}

function teardown() {
    enroot_cleanup sshd || true
}

# From https://github.com/NVIDIA/pyxis/issues/44
@test "ssh to container running sshd" {
    run_srun --container-image ubuntu:20.04 --container-name=sshd --container-remap-root bash -c 'apt-get update && apt-get install -y --no-install-recommends openssh-server'
    run_srun --container-name=sshd --no-container-remap-root --container-mount-home bash -c 'timeout --preserve-status --signal=TERM 10 /usr/sbin/sshd -d -p 2222' &

    sleep 3s
    # Force pseudo-terminal allocation with -t
    run_srun --overlap ssh -t -p 2222 -o StrictHostKeyChecking=no localhost true
    [ "${status}" -eq 0 ]

    wait
}

# From https://github.com/NVIDIA/pyxis/issues/45
@test "attach to container running sshd" {
    run_srun --container-image ubuntu:20.04 --container-name=sshd --container-remap-root bash -c 'apt-get update && apt-get install -y --no-install-recommends openssh-server'
    # Can't use timeout(1) here since we want the PID of sshd to appear in "enroot list -f"
    run_srun --container-name=sshd --no-container-remap-root -t 1 --signal TERM@30 /usr/sbin/sshd -d -p 2222 &

    sleep 3s
    run_srun --overlap --container-name=sshd true

    wait
}
