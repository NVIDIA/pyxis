#ifndef COMMON_H_
#define COMMON_H_

#include <unistd.h>
#include <stdio.h>
#include <stdbool.h>
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

#ifndef __STDC_NO_ATOMICS__
# if defined __has_include
#  if !__has_include(<stdatomic.h>)
#   define __STDC_NO_ATOMICS__
#  endif
# elif defined __GNUC__
#  define GCC_VERSION (__GNUC__*10000 + __GNUC_MINOR__*100 + __GNUC_PATCHLEVEL__)
#  if GCC_VERSION < 40900
#   define __STDC_NO_ATOMICS__
#  endif
# endif
#endif

#ifndef __STDC_NO_ATOMICS__
# include <stdatomic.h>
#else
typedef unsigned int atomic_uint;

# ifndef atomic_fetch_add
#  define atomic_fetch_add(PTR, VAL) __atomic_fetch_add((PTR), (VAL), __ATOMIC_SEQ_CST)
# endif
#endif

static inline int pyxis_memfd_create(const char *name, unsigned int flags)
{
	return syscall(__NR_memfd_create, name, flags);
}

char *get_line_from_file(FILE *fp);

char *join_strings(char *const strings[], const char *sep);

bool array_contains(char **array, size_t len, const char *entry);

int array_add_unique(char ***array, size_t *len, const char *entry);

void array_free(char ***array, size_t *len);

#endif /* COMMON_H_ */
