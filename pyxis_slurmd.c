/*
 * Copyright (c) 2019, NVIDIA CORPORATION. All rights reserved.
 */

#include <errno.h>
#include <ftw.h>
#include <string.h>

#include <slurm/spank.h>

#include "pyxis_slurmd.h"
#include "common.h"
#include "config.h"

int pyxis_slurmd_init(spank_t sp, int ac, char **av)
{
	int ret;
	struct plugin_config config;
	mode_t mask;
	int rv = -1;

	ret = pyxis_config_parse(&config, ac, av);
	if (ret < 0) {
		slurm_error("pyxis: failed to parse configuration");
		return (-1);
	}

	mask = umask(0);

	/* We only attempt to create the last component of the path. */
	ret = mkdir(config.runtime_path, 0755);
	if (ret < 0 && errno != EEXIST) {
		slurm_error("pyxis: slurmd: couldn't mkdir %s: %s", config.runtime_path, strerror(errno));
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
