/*
 * Copyright (c) 2021, NVIDIA CORPORATION. All rights reserved.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "pyxis_alloc.h"
#include "args.h"
#include "config.h"

struct plugin_context {
	struct plugin_args *args;
};

static struct plugin_context context = {
	.args = NULL,
};

int pyxis_alloc_init(spank_t sp, int ac, char **av)
{
	int ret;
	struct plugin_config config;

	ret = pyxis_config_parse(&config, ac, av);
	if (ret < 0) {
		slurm_error("pyxis: failed to parse configuration");
		return (-1);
	}

	if (!config.sbatch_support)
		return (0);

	context.args = pyxis_args_register(sp);
	if (context.args == NULL) {
		slurm_error("pyxis: failed to register arguments");
		return (-1);
	}

	return (0);
}

int pyxis_alloc_post_opt(spank_t sp, int ac, char **av)
{
	/* Calling pyxis_args_enabled() for arguments validation */
	pyxis_args_enabled();

	return (0);
}

int pyxis_alloc_exit(spank_t sp, int ac, char **av)
{
	pyxis_args_free();

	memset(&context, 0, sizeof(context));

	return (0);
}
