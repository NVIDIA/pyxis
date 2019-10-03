/*
 * Copyright (c) 2019, NVIDIA CORPORATION. All rights reserved.
 */

#include <errno.h>
#include <ftw.h>
#include <string.h>

#include <spank.h>

#include "common.h"

/*
 * Slurm bug: this SPANK callback is never called, instead slurm_spank_init is called with
 * context S_CTX_SLURMD.
 */
int slurm_spank_slurmd_init(spank_t sp, int ac, char **av)
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

int slurm_spank_slurmd_exit(spank_t sp, int ac, char **av)
{
	return (0);
}
