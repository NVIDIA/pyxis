#!/usr/bin/env bats

load ./common

@test "--container-mount-home --container-remap-root" {
    run_srun --container-mount-home --container-remap-root --container-image=ubuntu:18.04 findmnt /root
}

@test "--container-mount-home --no-container-remap-root" {
    run_srun --container-mount-home --no-container-remap-root --container-image=ubuntu:18.04 findmnt "${HOME}"
}

@test "--no-container-mount-home --container-remap-root" {
    run_srun --no-container-mount-home --container-remap-root --container-image=ubuntu:18.04 bash -c '! findmnt /root'
}

@test "--no-container-mount-home --no-container-remap-root" {
    run_srun --no-container-mount-home --no-container-remap-root --container-image=ubuntu:18.04 bash -c "! findmnt ${HOME}"
}
