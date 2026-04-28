#!/usr/bin/env bats

# Parser-level and cross-namespace tests for --container-unshare.
# Per-namespace tests live in unshare_net.bats, unshare_ipc.bats, and unshare_uts.bats.

load ./common

@test "--container-unshare=foo" {
    run_srun_unchecked --container-unshare=foo --container-image=ubuntu:24.04 true
    [ "${status}" -ne 0 ]
    grep -q "unknown namespace" <<< "${output}"
}

@test "--container-unshare=net,foo" {
    run_srun_unchecked --container-unshare=net,foo --container-image=ubuntu:24.04 true
    [ "${status}" -ne 0 ]
    grep -q "unknown namespace" <<< "${output}"
}

@test "--container-unshare=net,ipc,uts unshares all three" {
    host_netns=$(readlink /proc/self/ns/net)
    host_ipcns=$(readlink /proc/self/ns/ipc)
    host_utsns=$(readlink /proc/self/ns/uts)
    run_srun --container-unshare=net,ipc,uts --container-image=ubuntu:24.04 \
        bash -c 'echo "net=$(readlink /proc/self/ns/net)"; \
                 echo "ipc=$(readlink /proc/self/ns/ipc)"; \
                 echo "uts=$(readlink /proc/self/ns/uts)"'
    [ "$(grep '^net=' <<< "${output}" | cut -d= -f2)" != "${host_netns}" ]
    [ "$(grep '^ipc=' <<< "${output}" | cut -d= -f2)" != "${host_ipcns}" ]
    [ "$(grep '^uts=' <<< "${output}" | cut -d= -f2)" != "${host_utsns}" ]
}

@test "PYXIS_CONTAINER_UNSHARE=net env var" {
    host_netns=$(readlink /proc/self/ns/net)
    PYXIS_CONTAINER_UNSHARE=net run_srun --container-image=ubuntu:24.04 readlink /proc/self/ns/net
    [ "${lines[-1]}" != "${host_netns}" ]
}
