#!/usr/bin/env bats

load ./common

@test "--container-mounts absolute directory" {
    run_srun --container-mounts=/home:/test-mnt --container-image=ubuntu:18.04 findmnt /test-mnt
}

@test "--container-mounts regular file" {
    run_srun --container-mounts="${BASH_SOURCE[0]}:/script.bats" --container-image=ubuntu:18.04 cat /script.bats
}

@test "--container-mounts character device file" {
    run_srun --container-mounts="/dev/null:/null" --container-image=ubuntu:18.04 bash -c '[ -c /null ]'
}

@test "--container-mounts current working directory" {
    run_srun --container-mounts=./:/test-mnt --container-image=ubuntu:18.04 findmnt /test-mnt
}

@test "--container-mounts multiple entries" {
    run_srun --container-mounts=./:/test-mnt,/dev/null:/null,/home:/host/home --container-image=ubuntu:18.04 sh -c 'findmnt /test-mnt && findmnt /null && findmnt /host/home'
}

@test "--container-mounts /etc/shadow" {
    if [ "$(id -u)" -eq 0 ]; then
	skip "test requires non-root"
    fi
    # We should be able to bind-mount, but not access the file.
    run_srun --container-mounts=/etc/shadow:/shadow --container-image=ubuntu:18.04 sh -c 'findmnt /shadow'
    run_srun_unchecked --container-mounts=/etc/shadow:/shadow --container-image=ubuntu:18.04 cat /shadow
    [ "${status}" -ne 0 ]
    grep -q 'Permission denied' <<< "${output}"
}

@test "--container-mounts read-only directory" {
    run_srun_unchecked --container-mounts=/tmp:/mnt:ro --container-image=ubuntu:18.04 touch /mnt/foo
    [ "${status}" -ne 0 ]
    grep -q 'Read-only file system' <<< "${output}"
}

@test "--container-mounts multiple flags: unbindable+ro" {
    run_srun_unchecked --container-mounts=/tmp:/mnt:unbindable+ro --container-image=ubuntu:18.04 touch /mnt/foo
    [ "${status}" -ne 0 ]
    grep -q 'Read-only file system' <<< "${output}"

    run_srun --container-mounts=/tmp:/mnt:unbindable+ro --container-image=ubuntu:18.04 findmnt -o PROPAGATION /mnt
    grep -q 'unbindable' <<< "${lines[-1]}"
}

@test "--container-mounts path escape attempt" {
    run_srun_unchecked --container-mounts=/home:../home  --container-image=ubuntu:18.04 true
    [ "${status}" -ne 0 ]
    grep -q -i 'cross-device link' <<< "${output}"
}

@test "--container-mounts short-form" {
    run_srun --container-image=ubuntu:18.04 bash -c '! findmnt /var/lib'
    run_srun --container-mounts=/var/lib --container-image=ubuntu:18.04 findmnt /var/lib
}
