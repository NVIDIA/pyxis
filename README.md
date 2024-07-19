# Pyxis

Pyxis is a [SPANK](https://slurm.schedmd.com/spank.html) plugin for the Slurm Workload Manager.
It allows unprivileged cluster users to run containerized tasks through the `srun` command.

A pyxis is an [ancient small box or container](https://en.wikipedia.org/wiki/Pyxis_(vessel)).

## Benefits

* Seamlessly execute the user's task in an unprivileged container.
* Simple command-line interface.
* Fast Docker image download with support for layers caching and layers sharing across users.
* Supports multi-node MPI jobs through [PMI2](https://slurm.schedmd.com/mpi_guide.html) or [PMIx](https://pmix.org/) (requires Slurm support).
* Allows users to install packages inside the container.
* Works with shared filesystems.
* Does not require cluster-wide management of subordinate user/group ids.

## Installation
Pyxis requires the [enroot](https://github.com/nvidia/enroot) container utility (version `3.1.0`) to be installed.

Since [Slurm 21.08](https://github.com/SchedMD/slurm/blob/slurm-21-08-8-2/RELEASE_NOTES#L119-L121), pyxis must be compiled against the release of Slurm that is going to be deployed on the cluster. Compiling against `spank.h` from a different Slurm release will cause Slurm to prevent pyxis from loading with error `Incompatible plugin version`.

#### With `make install`
```console
$ sudo make install
$ sudo ln -s /usr/local/share/pyxis/pyxis.conf /etc/slurm/plugstack.conf.d/pyxis.conf
$ sudo systemctl restart slurmd
```

#### With a deb package
```console
$ make orig
$ make deb
$ sudo dpkg -i ../nvslurm-plugin-pyxis_*_amd64.deb
$ sudo ln -s /usr/share/pyxis/pyxis.conf /etc/slurm/plugstack.conf.d/pyxis.conf
$ sudo systemctl restart slurmd
```

#### With a rpm package
```console
$ make rpm
$ sudo rpm -i x86_64/nvslurm-plugin-pyxis-*-1.el7.x86_64.rpm
$ sudo ln -s /usr/share/pyxis/pyxis.conf /etc/slurm/plugstack.conf.d/pyxis.conf
$ sudo systemctl restart slurmd
```

## Usage
Pyxis being a SPANK plugin, the new command-line arguments it introduces are directly added to `srun`.

```console
$ srun --help
...
      --container-image=[USER@][REGISTRY#]IMAGE[:TAG]|PATH
                              [pyxis] the image to use for the container
                              filesystem. Can be either a docker image given as
                              an enroot URI, or a path to a squashfs file on the
                              remote host filesystem.

      --container-mounts=SRC:DST[:FLAGS][,SRC:DST...]
                              [pyxis] bind mount[s] inside the container. Mount
                              flags are separated with "+", e.g. "ro+rprivate"

      --container-workdir=PATH
                              [pyxis] working directory inside the container
      --container-name=NAME   [pyxis] name to use for saving and loading the
                              container on the host. Unnamed containers are
                              removed after the slurm task is complete; named
                              containers are not. If a container with this name
                              already exists, the existing container is used and
                              the import is skipped.
      --container-save=PATH   [pyxis] Save the container state to a squashfs
                              file on the remote host filesystem.
      --container-mount-home  [pyxis] bind mount the user's home directory.
                              System-level enroot settings might cause this
                              directory to be already-mounted.

      --no-container-mount-home
                              [pyxis] do not bind mount the user's home
                              directory
      --container-remap-root  [pyxis] ask to be remapped to root inside the
                              container. Does not grant elevated system
                              permissions, despite appearances.

      --no-container-remap-root
                              [pyxis] do not remap to root inside the container
      --container-entrypoint  [pyxis] execute the entrypoint from the container
                              image

      --no-container-entrypoint
                              [pyxis] do not execute the entrypoint from the
                              container image

      --container-entrypoint-log
                              [pyxis] print the output of the entrypoint script
      --container-writable    [pyxis] make the container filesystem writable
      --container-readonly    [pyxis] make the container filesystem read-only

      --container-env=NAME[,NAME...]
                              [pyxis] names of environment variables to override
                              with the host environment and set at the entrypoint.
                              By default, all exported host environment variables
                              are set in the container after the entrypoint is run,
                              but their existing values in the image take precedence;
                              the variables specified with this flag are preserved
                              from the host and set before the entrypoint runs
```

## Examples

### `srun`
```console
$ # Run a command on a worker node
$ srun grep PRETTY /etc/os-release
PRETTY_NAME="Ubuntu 20.04.2 LTS"

$ # run the same command, but now inside of a container
$ srun --container-image=centos grep PRETTY /etc/os-release
PRETTY_NAME="CentOS Linux 8"

$ # mount a file from the host and run the command on it, from inside the container
$ srun --container-image=centos --container-mounts=/etc/os-release:/host/os-release grep PRETTY /host/os-release
PRETTY_NAME="Ubuntu 20.04.2 LTS"
```

### `sbatch`
```console
$ # execute the sbatch script inside a container image
$ sbatch --wait -o slurm.out <<EOF
#!/bin/bash
#SBATCH --container-image nvcr.io\#nvidia/pytorch:21.12-py3

python -c 'import torch ; print(torch.__version__)'
EOF

$ cat slurm.out
pyxis: importing docker image: nvcr.io#nvidia/pytorch:21.12-py3
1.11.0a0+b6df043
```
As `#` is the character used to start a `SBATCH` comment, this character needs to be escaped when also used in `--container-image` as a separator between the registry and the image name.

## Advanced Documentation (wiki)
1. [Home](https://github.com/NVIDIA/pyxis/wiki/Home)
1. [Installation](https://github.com/NVIDIA/pyxis/wiki/Installation)
1. [Setup](https://github.com/NVIDIA/pyxis/wiki/Setup)
1. [Usage](https://github.com/NVIDIA/pyxis/wiki/Usage)

## Copyright and License

This project is released under the [Apache License Version 2.0](https://github.com/NVIDIA/pyxis/blob/master/LICENSE).

## Issues and Contributing

* Please let us know by [filing a new issue](https://github.com/NVIDIA/pyxis/issues/new)
* Check [CONTRIBUTING](CONTRIBUTING.md) and then open a [pull request](https://help.github.com/articles/using-pull-requests/)

## Running tests
Integration tests can be ran with [bats](https://github.com/bats-core/bats-core) from within a Slurm job allocation:
```console
$ salloc --overcommit bats tests
$ bats tests/sbatch
```
Some tests assume a specific enroot configuration (such as PMIx/PyTorch hooks), so they might not pass on all systems.

## Reporting Security Issues

When reporting a security issue, do not create an issue or file a pull request.  
Instead, disclose the issue responsibly by sending an email to `psirt<at>nvidia.com`.
