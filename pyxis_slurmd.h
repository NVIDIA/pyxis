/*
 * Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
 */

#ifndef PYXIS_SLURMD_H_
#define PYXIS_SLURMD_H_

#include <slurm/spank.h>

int pyxis_slurmd_init(spank_t sp, int ac, char **av);

int pyxis_slurmd_exit(spank_t sp, int ac, char **av);

#endif /* PYXIS_SLURMD_H_ */
