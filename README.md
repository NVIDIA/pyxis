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

```console
$ # Option 1: quick install from sources
$ sudo make install
$ sudo ln -s /usr/local/share/pyxis/pyxis.conf /etc/slurm-llnl/plugstack.conf.d/pyxis.conf
$ sudo systemctl restart slurmd

$ # Option 2: generate a deb package and install it
$ make orig
$ make deb
$ sudo dpkg -i ../nvslurm-plugin-pyxis_*_amd64.deb
$ sudo ln -s /usr/share/pyxis/pyxis.conf /etc/slurm-llnl/plugstack.conf.d/pyxis.conf
$ sudo systemctl restart slurmd
```

## Usage
Pyxis being a SPANK plugin, the new command-line arguments it introduces are directly added to `srun`.

```
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
```

## Examples

```console
$ # Run a command on a worker node
$ srun grep PRETTY /etc/os-release
PRETTY_NAME="Ubuntu 18.04.2 LTS"

$ # run the same command, but now inside of a container
$ srun --container-image=centos grep PRETTY /etc/os-release
PRETTY_NAME="CentOS Linux 7 (Core)"

$ # mount a file from the host and run the command on it, from inside the container
$ srun --container-image=centos --container-mounts=/etc/os-release:/host/os-release grep PRETTY /host/os-release
PRETTY_NAME="Ubuntu 18.04.2 LTS"
```

## Copyright and License

This project is released under the [Apache License Version 2.0](https://github.com/NVIDIA/pyxis/blob/master/LICENSE).

## Issues and Contributing

* Please let us know by [filing a new issue](https://github.com/NVIDIA/pyxis/issues/new)
* Check [CONTRIBUTING](CONTRIBUTING.md) and then open a [pull request](https://help.github.com/articles/using-pull-requests/)

## Reporting Security Issues

When reporting a security issue, do not create an issue or file a pull request.  
Instead, disclose the issue responsibly by sending an email to `psirt<at>nvidia.com`.
