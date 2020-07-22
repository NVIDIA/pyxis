/*
 * Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
 */

#ifndef CONFIG_H_
#define CONFIG_H_

#include <limits.h>
#include <stdbool.h>

struct plugin_config {
	char runtime_path[PATH_MAX];
	bool remap_root;
	bool execute_entrypoint;
};

int pyxis_config_parse(struct plugin_config *config, int ac, char **av);

#endif /* CONFIG_H_ */
