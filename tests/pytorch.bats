#!/usr/bin/env bats

load ./common

function setup() {
    enroot_cleanup pytorch-test || true
}

function teardown() {
    enroot_cleanup pytorch-test || true
}

@test "nvcr.io PyTorch environment variables" {
    run_srun --mpi=none --container-name=pytorch-test --container-image=nvcr.io#nvidia/pytorch:20.02-py3 true
    run_srun --mpi=none --ntasks=4 --container-name=pytorch-test bash -c '[ -n "${MASTER_ADDR}" ]'
    run_srun --mpi=none --ntasks=4 --container-name=pytorch-test bash -c '[ -n "${MASTER_PORT}" ]'
    run_srun --mpi=none --ntasks=4 --container-name=pytorch-test bash -c '[ -n "${LOCAL_RANK}" ] && [ "${LOCAL_RANK}" -eq "${SLURM_LOCALID}" ]'
    run_srun --mpi=none --ntasks=4 --container-name=pytorch-test bash -c '[ -n "${RANK}" ] && [ "${RANK}" -eq "${SLURM_PROCID}" ]'
}

@test "Docker Hub PyTorch environment variables" {
    run_srun --mpi=none --container-name=pytorch-test --container-image=pytorch/pytorch:1.8.1-cuda11.1-cudnn8-runtime true
    run_srun --mpi=none --ntasks=6 --container-name=pytorch-test bash -c '[ -n "${LOCAL_RANK}" ] && [ "${LOCAL_RANK}" -eq "${SLURM_LOCALID}" ]'
    run_srun --mpi=none --ntasks=6 --container-name=pytorch-test bash -c '[ -n "${RANK}" ] && [ "${RANK}" -eq "${SLURM_PROCID}" ]'

    run_srun --mpi=none --ntasks=6 --container-name=pytorch-test bash -c 'echo ${LOCAL_RANK}'
    [ "$(sort -n <<< ${output})" == "$(seq 0 5)" ]
}
