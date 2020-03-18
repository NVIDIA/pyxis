#!/usr/bin/env bats

load ./common

@test "Docker Hub ubuntu:16.04" {
    run_srun --container-image=ubuntu:16.04 grep 'Ubuntu 16.04' /etc/os-release
}

@test "Docker Hub ubuntu:18.04" {
    run_srun --container-image=ubuntu:18.04 grep 'Ubuntu 18.04' /etc/os-release
}

@test "Docker Hub centos:7" {
    run_srun --container-image=centos:7 grep 'CentOS Linux release 7.' /etc/redhat-release
}

@test "Docker Hub centos:8" {
    run_srun --container-image=centos:8 grep 'CentOS Linux release 8.' /etc/redhat-release
}

@test "nvcr.io PyTorch 20.02" {
    run_srun --container-image=nvcr.io#nvidia/pytorch:20.02-py3 sh -c 'echo $PYTORCH_VERSION'
    [ "${lines[-1]}" == "1.5.0a0+3bbb36e" ]
}

@test "gcr.io TensorFlow 1.14" {
    run_srun --no-container-mount-home --container-image=gcr.io#deeplearning-platform-release/tf-gpu.1-14 /entrypoint.sh python -c 'import tensorflow; print(tensorflow.__version__)'
    [ "${lines[-1]}" == "1.14.0" ]
}

@test "gitlab.com NVIDIA device plugin" {
    run_srun --container-image=registry.gitlab.com#nvidia/kubernetes/device-plugin/k8s-device-plugin:1.0.0-beta4 md5sum /usr/bin/nvidia-device-plugin
    [ "${lines[-1]}" == "7d9c8e3e005c4bd2161def017f10f719  /usr/bin/nvidia-device-plugin" ]
}
