#!/usr/bin/env bats

load ./common

@test "Docker Hub ubuntu:12.04" {
    run_srun --container-image=ubuntu:12.04 grep 'Ubuntu precise' /etc/os-release
}

@test "Docker Hub ubuntu:14.04" {
    run_srun --container-image=ubuntu:14.04 grep 'Ubuntu 14.04' /etc/os-release
}

@test "Docker Hub ubuntu:16.04" {
    run_srun --container-image=ubuntu:16.04 grep 'Ubuntu 16.04' /etc/os-release
}

@test "Docker Hub ubuntu:18.04" {
    run_srun --container-image=ubuntu:18.04 grep 'Ubuntu 18.04' /etc/os-release
}

@test "Docker Hub ubuntu:20.04" {
    run_srun --container-image=ubuntu:20.04 grep 'Ubuntu 20.04' /etc/os-release
}

@test "Docker Hub ubuntu:22.04" {
    run_srun --container-image=ubuntu:22.04 grep 'Ubuntu 22.04' /etc/os-release
}

@test "Docker Hub centos:5" {
    run_srun --container-image=centos:5 grep 'CentOS release 5.11 (Final)' /etc/redhat-release
}

@test "Docker Hub centos:6" {
    run_srun --container-image=centos:6 grep 'CentOS release 6.10 (Final)' /etc/redhat-release
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

@test "nvcr.io PyTorch 22.04" {
    run_srun --container-image=nvcr.io#nvidia/pytorch:22.04-py3 sh -c 'echo $PYTORCH_VERSION'
    [ "${lines[-1]}" == "1.12.0a0+bd13bc6" ]
}

@test "gcr.io TensorFlow 1.14" {
    run_srun --no-container-mount-home --container-image=gcr.io#deeplearning-platform-release/tf-gpu.1-14 /entrypoint.sh python -c 'import tensorflow; print(tensorflow.__version__)'
    [ "${lines[-1]}" == "1.14.0" ]
}

@test "gcr.io TensorFlow 2.8" {
    run_srun --no-container-mount-home --container-image=gcr.io#deeplearning-platform-release/tf-gpu.2-8 /entrypoint.sh python -c 'import tensorflow; print(tensorflow.__version__)'
    [ "${lines[-1]}" == "2.8.0" ]
}

@test "gitlab.com NVIDIA device plugin" {
    run_srun --container-image=registry.gitlab.com#nvidia/kubernetes/device-plugin/k8s-device-plugin:1.0.0-beta4 md5sum /usr/bin/nvidia-device-plugin
    [ "${lines[-1]}" == "7d9c8e3e005c4bd2161def017f10f719  /usr/bin/nvidia-device-plugin" ]
}

@test "nvcr.io NVIDIA device plugin" {
    run_srun --container-image=nvcr.io#nvidia/k8s-device-plugin:v0.11.0 md5sum /usr/bin/nvidia-device-plugin
    [ "${lines[-1]}" == "78d5dfab092f182246d7e1aff420b770  /usr/bin/nvidia-device-plugin" ]
}

@test "image download must be attempted once" {
    run_srun_unchecked --ntasks=2 --container-image=thisimagedoesntexist true
    [ "${status}" -ne 0 ]
    attempts=$(grep -c 'failed to import docker image' <<< "${output}")
    [ "${attempts}" -eq 1 ]
}
