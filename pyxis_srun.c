/*
 * Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <slurm/spank.h>

#include "pyxis_srun.h"
#include "args.h"
#include "config.h"

struct plugin_context {
	struct plugin_args *args;
	struct plugin_config config;
};

static struct plugin_context context = {
	.args = NULL,
};

int pyxis_srun_init(spank_t sp, int ac, char **av)
{
	context.args = pyxis_args_register(sp);
	if (context.args == NULL) {
		slurm_error("pyxis: failed to register arguments");
		return (-1);
	}

	return (0);
}

int pyxis_srun_post_opt(spank_t sp, int ac, char **av)
{
	/* Calling pyxis_args_enabled() for arguments validation */
	pyxis_args_enabled();

	int ret;
	ret = pyxis_config_parse(&context.config, ac, av);
	if (ret < 0) {
		slurm_error("pyxis: failed to parse configuration");
		return (-1);
	}

	if (!pyxis_args_valid(context.config)) {
		return (-1);
	}

	return (0);
}

int pyxis_srun_exit(spank_t sp, int ac, char **av)
{
	pyxis_args_free();

	memset(&context, 0, sizeof(context));

	return (0);
}


