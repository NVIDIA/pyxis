#!/bin/bash

function log() {
    echo "${@}" >&2
}

function logf() {
    printf "${@}\n" >&2
}

function run_enroot() {
    log "+ srun -N1 --oversubscribe enroot $@"
    run srun -N1 --oversubscribe enroot "$@"

    log "${output}"

    echo "+ exit status: ${status}"
    [ "${status}" -eq 0 ]
}

function run_srun_unchecked() {
    log "+ srun -N1 --oversubscribe $@"
    run srun -N1 --oversubscribe "$@"

    log "${output}"

    echo "+ exit status: ${status}"
}

function run_srun() {
    run_srun_unchecked "$@"
    [ "${status}" -eq 0 ]
}

function run_sbatch_unchecked() {
    log "+ sbatch --wait $@"

    slurm_log=$(mktemp)
    run sbatch --wait -o "${slurm_log}" "$@"

    log "${output}"
    logf "+ job log (${slurm_log}):\n$(cat ${slurm_log})"
    rm -f "${slurm_log}"

    echo "+ exit status: ${status}"
}

function run_sbatch() {
    run_sbatch_unchecked "$@"
    [ "${status}" -eq 0 ]
}

function enroot_cleanup() {
    for name in "$@"; do
	{ srun enroot remove -f pyxis_${name} || srun enroot remove -f pyxis_${SLURM_JOB_ID}_${name}; } >/dev/null 2>&1
    done
}
