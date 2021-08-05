#!/usr/bin/env bats

load ./common

@test "passthrough of environment variable" {
    export FOO=bar
    run_srun --container-image=ubuntu:18.04 sh -c 'echo $FOO'
    [ "${lines[-1]}" == "bar" ]
}

@test "\$TERM special case" {
    TERM=dumb run_srun --no-container-mount-home --container-image=ubuntu:18.04 sh -c 'echo $TERM'
    [ "${lines[-1]}" == "dumb" ]
}

@test "\$HOME special case" {
    HOME=/tmp run_srun --container-mount-home --container-remap-root --container-image=ubuntu:18.04 findmnt /root -o SOURCE
    grep -q '/tmp' <<< "${lines[-1]}" || grep -q 'tmpfs' <<< "${lines[-1]}"
}

@test "nvidia/cuda:10.2-base with \$NVIDIA_VISIBLE_DEVICES=0" {
    if ! srun which nvidia-smi; then
	skip "no NVIDIA GPUs"
    fi
    gpus="$(srun nvidia-smi -L | wc -l)"
    if [ "${gpus}" -le 1 ]; then
	skip "need 2 or more GPUs"
    fi

    run_srun --container-image=nvidia/cuda:10.2-base bash -c "nvidia-smi -L | wc -l"
    [ "${lines[-1]}" -eq "${gpus}" ]

    NVIDIA_VISIBLE_DEVICES=0 run_srun --container-image=nvidia/cuda:10.2-base bash -c "nvidia-smi -L | wc -l"
    [ "${lines[-1]}" -eq 1 ]
}

@test "ubuntu:18.04 with \$NVIDIA_VISIBLE_DEVICES=all" {
    if ! srun which nvidia-smi; then
	skip "no NVIDIA GPUs"
    fi

    NVIDIA_VISIBLE_DEVICES=all run_srun --container-image=ubuntu nvidia-smi
}

@test "ubuntu:18.04 with \$NVIDIA_DRIVER_CAPABILITIES=utility,compute" {
    if ! srun which nvidia-smi; then
	skip "no NVIDIA GPUs"
    fi

    NVIDIA_VISIBLE_DEVICES=all NVIDIA_DRIVER_CAPABILITIES=utility,compute run_srun --container-image=ubuntu bash -c 'ldconfig -p | grep libcuda.so.1'
}
