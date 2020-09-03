#!/usr/bin/env bats

load ./common

function setup() {
    enroot remove -f pyxis_${SLURM_JOB_ID}_nginx-test >/dev/null 2>&1 || true
}

function teardown() {
    enroot remove -f pyxis_${SLURM_JOB_ID}_nginx-test >/dev/null 2>&1 || true
}

@test "--container-entrypoint: jrottenberg/ffmpeg" {
    # This container image uses 'ENTRYPOINT ["ffmpeg"]'
    run_srun_unchecked --container-entrypoint --container-image jrottenberg/ffmpeg:3.2-alpine true
    [ "${status}" -ne 0 ]
    run_srun_unchecked --no-container-entrypoint --container-image jrottenberg/ffmpeg:3.2-alpine true
    [ "${status}" -eq 0 ]
}

@test "manual execution of entrypoint" {
    # See https://github.com/nginxinc/docker-nginx/blob/1.19.0/mainline/buster/10-listen-on-ipv6-by-default.sh
    run_srun --container-image nginx:1.19.1 --container-name nginx-test --container-remap-root nginx -v
    run_srun --container-name nginx-test cat /etc/nginx/conf.d/default.conf
    grep -q "listen       80;" <<< "${output}"

    run_srun --container-name nginx-test --container-remap-root /docker-entrypoint.sh nginx -v
    run_srun --container-name nginx-test cat /etc/nginx/conf.d/default.conf
    grep -q "listen  \[::]\:80;" <<< "${output}"
}
