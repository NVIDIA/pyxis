/*
 * Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
 */

#ifndef PYXIS_SRUN_H_
#define PYXIS_SRUN_H_

#include <slurm/spank.h>

int pyxis_srun_init(spank_t sp, int ac, char **av);

int pyxis_srun_post_opt(spank_t sp, int ac, char **av);

int pyxis_srun_exit(spank_t sp, int ac, char **av);

#endif /* PYXIS_SRUN_H_ */
