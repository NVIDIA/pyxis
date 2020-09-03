#!/usr/bin/env bats

load ./common

function setup() {
    enroot remove -f pyxis_${SLURM_JOB_ID}_pytorch-test || true
}

function teardown() {
    enroot remove -f pyxis_${SLURM_JOB_ID}_pytorch-test || true
}

@test "PyTorch environment variables" {
    run_srun --mpi=none --container-name=pytorch-test --container-image=nvcr.io#nvidia/pytorch:20.02-py3 true
    run_srun --mpi=none --ntasks=4 --container-name=pytorch-test bash -c '[ -n "${MASTER_ADDR}" ]'
    run_srun --mpi=none --ntasks=4 --container-name=pytorch-test bash -c '[ -n "${MASTER_PORT}" ]'
    run_srun --mpi=none --ntasks=4 --container-name=pytorch-test bash -c '[ -n "${LOCAL_RANK}" ] && [ "${LOCAL_RANK}" -eq "${SLURM_LOCALID}" ]'
    run_srun --mpi=none --ntasks=4 --container-name=pytorch-test bash -c '[ -n "${RANK}" ] && [ "${RANK}" -eq "${SLURM_PROCID}" ]'
}
