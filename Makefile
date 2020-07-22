prefix      ?= /usr/local
libdir      ?= $(prefix)/lib
datarootdir ?= $(prefix)/share
datadir     ?= $(datarootdir)

PLUGINDIR := $(abspath $(DESTDIR)/$(libdir)/slurm)
CONFDIR   := $(abspath $(DESTDIR)/$(datadir)/pyxis)

PLUGIN := spank_pyxis.so
CONF   := pyxis.conf

.PHONY: all install uninstall clean deb

CPPFLAGS := -D_GNU_SOURCE -D_FORTIFY_SOURCE=2 $(CPPFLAGS)
CFLAGS := -std=gnu11 -O2 -g -Wall -Wunused-variable -fstack-protector-strong -fpic $(CFLAGS)
LDFLAGS := -Wl,-znoexecstack -Wl,-zrelro -Wl,-znow $(LDFLAGS)

C_SRCS := common.c args.c pyxis_slurmstepd.c pyxis_slurmd.c pyxis_srun.c pyxis_dispatch.c config.c
C_OBJS := $(C_SRCS:.c=.o)

DEPS := $(C_OBJS:%.o=%.d)

all: $(PLUGIN)

$(C_OBJS): %.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MF $*.d -c $<

$(PLUGIN): $(C_OBJS)
	$(CC) -shared $(LDFLAGS) -o $@ spank_pyxis.lds $^
	strip --strip-unneeded -R .comment $@

install: all
	install -d -m 755 $(PLUGINDIR)
	install -m 644 $(PLUGIN) $(PLUGINDIR)
	install -d -m 755 $(CONFDIR)
	echo 'required $(libdir)/slurm/$(PLUGIN)' | install -m 644 /dev/stdin $(CONFDIR)/$(CONF)

uninstall:
	$(RM) $(PLUGINDIR)/$(PLUGIN)
	$(RM) $(CONFDIR)/$(CONF)

clean:
	rm -rf $(C_OBJS) $(DEPS) $(PLUGIN)

orig: clean
	tar -caf ../nvslurm-plugin-pyxis_0.7.0.orig.tar.xz --owner=root --group=root --exclude=.git .

deb: clean
	debuild -us -uc -G -i -tc

-include $(DEPS)
