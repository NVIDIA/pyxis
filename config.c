/*
 * Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
 */

#include <string.h>

#include <slurm/spank.h>

#include "config.h"

int pyxis_config_parse(struct plugin_config *config, int ac, char **av)
{
	const char *optarg;

	memset(config, 0, sizeof(*config));

	/*
	 * Since Slurm might not be configured to integrate with PAM and
	 * logind, we can't assume /run/user/<uid> will be present.
	 * Instead, we default to using a new directory under an existing tmpfs: /run/pyxis.
	 */
	strcpy(config->runtime_path, "/run/pyxis");

	for (int i = 0; i < ac; ++i) {
		if (strncmp("runtime_path=", av[i], 13) == 0) {
			optarg = av[i] + 13;
			if (memccpy(config->runtime_path, optarg, '\0', sizeof(config->runtime_path)) == NULL) {
				slurm_error("pyxis: runtime_path: path too long: %s", optarg);
				return (-1);
			}
		} else {
			slurm_error("pyxis: unknown configuration option: %s", av[i]);
			return (-1);
		}
	}

	return (0);
}
