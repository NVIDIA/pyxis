# Pyxis Importer Plugin

Pyxis supports an importer plugin mechanism to fetch squashfs files for container images. This
mechanism allows administrators to customize how Docker images are imported, enabling features like
caching, optimized storage, or integration with custom image registries.

## Configuration

To enable an importer plugin, configure pyxis with the `importer=` option in `plugstack.conf`:

```
required pyxis.so importer=/path/to/importer/script.sh
```

When an importer is configured, pyxis will use it instead of the built-in `enroot import` (or `enroot load`).

## Plugin Interface

An importer plugin is an executable that implements two operations: `get` and `release`.

### `get` Operation

The `get` operation is called to fetch a Docker image and produce a squashfs file.

**Usage:**
```bash
importer get IMAGE_URI
```

**Arguments:**
* `IMAGE_URI`: The image to fetch, in enroot URI format (e.g., `docker://ubuntu:24.04`)

**Output:**
The plugin must write the absolute path to the squashfs file on stdout. Any progress messages or
diagnostic output should be written to stderr. If the importer terminates with a non-zero exit code,
pyxis will print the stderr log of the importer.

**Environment:**
The plugin runs with the same environment variables available to enroot, plus:
* `PYXIS_RUNTIME_PATH`: The `runtime_path` configured for pyxis
* `PYXIS_VERSION`: The version of pyxis
* Standard Slurm environment variables (`SLURM_JOB_UID`, `SLURM_JOB_ID`, `SLURM_STEP_ID`, etc.)

### `release` Operation

The `release` operation is called to clean up resources associated with the squashfs file.

**Usage:**
```bash
importer release
```

**Important:** To handle cases where the job step receives `SIGKILL`. The `release` operation is often called **twice** during the lifecycle of a job:
1. Once after the container is created
2. Once during task cleanup

Implementations must be idempotent and handle being called multiple times safely. The plugin should not fail if resources have already been cleaned up.

**Environment:**
The same environment variables as the `get` operation are available. The plugin should use these
variables (e.g. `SLURM_JOB_ID` and `SLURM_STEP_ID`) to identify which resources to release.

## Example Implementations

Two example importer scripts are provided in this directory:

### `simple_importer.sh`

A straightforward implementation, similar to what pyxis is doing natively:
* Uses `enroot import` to fetch images into a per-job squashfs file
* Stores files in `${PYXIS_RUNTIME_PATH}/${SLURM_JOB_UID}/${SLURM_JOB_ID}.${SLURM_STEP_ID}.squashfs`
* Removes the squashfs file on `release`

This implementation is suitable for basic use cases where each job gets its own image copy.

### `caching_importer.sh`

An example of a more advanced implementation that:
* Uses image digests to enable sharing of squashfs files across jobs
* Caches imported images in `/tmp/pyxis-cache-${SLURM_JOB_UID}` by digest
* Enables squashfs compression (`zstd`) since cached files are long-lived
* Only removes temporary files on `release`, keeping the cache intact for reuse

This implementation could be suitable for environments where multiple jobs use the same container
images, as it avoids redundant downloads and storage. It does not handle locking of the cache
directory so it might not be suitable for all filesystems.
