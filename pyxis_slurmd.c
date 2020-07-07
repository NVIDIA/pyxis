/*
 * Copyright (c) 2019, NVIDIA CORPORATION. All rights reserved.
 */

#include <errno.h>
#include <ftw.h>
#include <string.h>

#include <slurm/spank.h>

#include "pyxis_slurmd.h"
#include "common.h"

int pyxis_slurmd_init(spank_t sp, int ac, char **av)
{
	int ret;
	mode_t mask;
	int rv = -1;

	mask = umask(0);

	/*
	 * Since Slurm might not be configured to integrate with PAM and
	 * logind, we can't assume /run/user/<uid> will be present.
	 * Instead, we create a new directory under an existing tmpfs (e.g. /run/pyxis).
	 */
	ret = mkdir(PYXIS_RUNTIME_PATH, 0755);
	if (ret < 0 && errno != EEXIST) {
		slurm_error("pyxis: slurmd: couldn't mkdir %s: %s", PYXIS_RUNTIME_PATH, strerror(errno));
		goto fail;
	}

	rv = 0;

fail:
	(void)umask(mask);

	return (rv);
}

int pyxis_slurmd_exit(spank_t sp, int ac, char **av)
{
	return (0);
}
