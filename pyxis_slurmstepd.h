/*
 * Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
 */

#ifndef PYXIS_SLURMSTEPD_H_
#define PYXIS_SLURMSTEPD_H_

#include <slurm/spank.h>

int pyxis_slurmstepd_init(spank_t sp, int ac, char **av);

int pyxis_slurmstepd_post_opt(spank_t sp, int ac, char **av);

int pyxis_slurmstepd_exit(spank_t sp, int ac, char **av);

#endif /* PYXIS_SLURMSTEPD_H_ */
