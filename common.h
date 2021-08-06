#ifndef COMMON_H_
#define COMMON_H_

#include <unistd.h>
#include <stdio.h>
#include <sys/syscall.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(*x))

static inline void xclose(int fd)
{
	if (fd < 0)
		return;
	close(fd);
}

int xasprintf(char **strp, const char *fmt, ...);

/* https://github.com/SchedMD/slurm/blob/slurm-20-11-8-1/slurm/slurm.h.in#L161-L162 */
#if !defined(SLURM_BATCH_SCRIPT)
# define SLURM_BATCH_SCRIPT (0xfffffffb)
#endif

#if !defined(MFD_CLOEXEC)
# define MFD_CLOEXEC 0x0001U
#endif /* !defined(MFD_CLOEXEC) */

#if defined(__x86_64__)
# if !defined(__NR_memfd_create)
#  define __NR_memfd_create 319
# endif /* !defined(__NR_memfd_create) */
#endif /* defined(__x86_64__) */

#ifndef CLONE_NEWCGROUP
# define CLONE_NEWCGROUP 0x02000000
#endif

static inline int pyxis_memfd_create(const char *name, unsigned int flags)
{
	return syscall(__NR_memfd_create, name, flags);
}

char *get_line_from_file(FILE *fp);

char *join_strings(char *const strings[], const char *sep);

#endif /* COMMON_H_ */
