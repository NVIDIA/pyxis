/*
 * Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
 */

#include <string.h>

#include <slurm/spank.h>

#include "config.h"

static int parse_bool(const char *s)
{
	if (strcmp(s, "1") == 0 || strcmp(s, "true") == 0)
		return (true);

	if (strcmp(s, "0") == 0 || strcmp(s, "false") == 0)
		return (false);

	return (-1);
}

int pyxis_config_parse(struct plugin_config *config, int ac, char **av)
{
	int ret;
	const char *optarg;

	memset(config, 0, sizeof(*config));

	/*
	 * Since Slurm might not be configured to integrate with PAM and
	 * logind, we can't assume /run/user/<uid> will be present.
	 * Instead, we default to using a new directory under an existing tmpfs: /run/pyxis.
	 */
	strcpy(config->runtime_path, "/run/pyxis");
	config->remap_root = true;
	config->execute_entrypoint = false;
	config->epilog = true;

	for (int i = 0; i < ac; ++i) {
		if (strncmp("runtime_path=", av[i], 13) == 0) {
			optarg = av[i] + 13;
			if (memccpy(config->runtime_path, optarg, '\0', sizeof(config->runtime_path)) == NULL) {
				slurm_error("pyxis: runtime_path: path too long: %s", optarg);
				return (-1);
			}
		} else if (strncmp("remap_root=", av[i], 11) == 0) {
			optarg = av[i] + 11;
			ret = parse_bool(optarg);
			if (ret < 0) {
				slurm_error("pyxis: remap_root: invalid value: %s", optarg);
				return (-1);
			}
			config->remap_root = ret;
		} else if (strncmp("execute_entrypoint=", av[i], 19) == 0) {
			optarg = av[i] + 19;
			ret = parse_bool(optarg);
			if (ret < 0) {
				slurm_error("pyxis: execute_entrypoint: invalid value: %s", optarg);
				return (-1);
			}
			config->execute_entrypoint = ret;
		} else if (strncmp("epilog=", av[i], 7) == 0) {
			optarg = av[i] + 7;
			ret = parse_bool(optarg);
			if (ret < 0) {
				slurm_error("pyxis: epilog: invalid value: %s", optarg);
				return (-1);
			}
			config->epilog = ret;
		} else {
			slurm_error("pyxis: unknown configuration option: %s", av[i]);
			return (-1);
		}
	}

	return (0);
}
