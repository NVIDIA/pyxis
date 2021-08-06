/*
 * Copyright (c) 2020, NVIDIA CORPORATION. All rights reserved.
 */

#include <slurm/spank.h>

#include "pyxis_slurmd.h"
#include "pyxis_srun.h"
#include "pyxis_alloc.h"
#include "pyxis_slurmstepd.h"

SPANK_PLUGIN(pyxis, 1)

int slurm_spank_init(spank_t sp, int ac, char **av)
{
	switch (spank_context()) {
	case S_CTX_SLURMD:
		return pyxis_slurmd_init(sp, ac, av);
	case S_CTX_LOCAL:
		return pyxis_srun_init(sp, ac, av);
	case S_CTX_ALLOCATOR:
		return pyxis_alloc_init(sp, ac, av);
	case S_CTX_REMOTE:
		return pyxis_slurmstepd_init(sp, ac, av);
	default:
		return (0);
	}
}

int slurm_spank_init_post_opt(spank_t sp, int ac, char **av)
{
	switch (spank_context()) {
	case S_CTX_LOCAL:
		return pyxis_srun_post_opt(sp, ac, av);
	case S_CTX_ALLOCATOR:
		return pyxis_alloc_post_opt(sp, ac, av);
	case S_CTX_REMOTE:
		return pyxis_slurmstepd_post_opt(sp, ac, av);
	default:
		return (0);
	}
}

int slurm_spank_exit(spank_t sp, int ac, char **av)
{
	switch (spank_context()) {
	case S_CTX_SLURMD:
		return pyxis_slurmd_exit(sp, ac, av);
	case S_CTX_LOCAL:
		return pyxis_srun_exit(sp, ac, av);
	case S_CTX_ALLOCATOR:
		return pyxis_alloc_exit(sp, ac, av);
	case S_CTX_REMOTE:
		return pyxis_slurmstepd_exit(sp, ac, av);
	default:
		return (0);
	}
}

int slurm_spank_slurmd_exit(spank_t sp, int ac, char **av)
{
	return pyxis_slurmd_exit(sp, ac, av);
}
