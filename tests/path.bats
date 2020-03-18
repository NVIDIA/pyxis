#!/usr/bin/env bats

load ./common

# Check if Slurm can find binaries that only exist inside the container filesystem
# See https://bugs.schedmd.com/show_bug.cgi?id=7257

@test "redis-server PATH resolution" {
    if which redis-server; then
	skip "redis-server present locally"
    fi
    run_srun --container-image=redis:5.0.8 redis-server --version
}

@test "rabbitmq PATH resolution" {
    if which rabbitmqctl; then
	skip "rabbitmqctl present locally"
    fi
    run_srun --container-image=rabbitmq:3.8.3 rabbitmqctl version
}
