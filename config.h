/*
 * Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
 */

#ifndef CONFIG_H_
#define CONFIG_H_

#include <limits.h>
#include <stdbool.h>

enum container_scope {
	SCOPE_JOB,
	SCOPE_GLOBAL,
};

struct plugin_config {
	char runtime_path[PATH_MAX];
	bool execute_entrypoint;
	enum container_scope container_scope;
	bool sbatch_support;
	bool use_enroot_load;
};

int pyxis_config_parse(struct plugin_config *config, int ac, char **av);

int parse_bool(const char *s);

#endif /* CONFIG_H_ */
