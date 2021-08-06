/*
 * Copyright (c) 2021, NVIDIA CORPORATION. All rights reserved.
 */

#ifndef PYXIS_ALLOC_H_
#define PYXIS_ALLOC_H_

#include <slurm/spank.h>

int pyxis_alloc_init(spank_t sp, int ac, char **av);

int pyxis_alloc_post_opt(spank_t sp, int ac, char **av);

int pyxis_alloc_exit(spank_t sp, int ac, char **av);

#endif /* PYXIS_ALLOC_H_ */
