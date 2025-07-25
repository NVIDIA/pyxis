prefix      ?= /usr/local
libdir      ?= $(prefix)/lib
datarootdir ?= $(prefix)/share
datadir     ?= $(datarootdir)

PLUGINDIR := $(abspath $(DESTDIR)/$(libdir)/slurm)
CONFDIR   := $(abspath $(DESTDIR)/$(datadir)/pyxis)

PYXIS_VER ?= 0.20.0

PLUGIN := spank_pyxis.so
CONF   := pyxis.conf

.PHONY: all install uninstall clean deb rpm

CPPFLAGS := -D_GNU_SOURCE -D_FORTIFY_SOURCE=2 -DPYXIS_VERSION=\"$(PYXIS_VER)\" $(CPPFLAGS)
CFLAGS := -std=gnu11 -O2 -g -Wall -Wunused-variable -fstack-protector-strong -fpic $(CFLAGS)
LDFLAGS := -Wl,-znoexecstack -Wl,-zrelro -Wl,-znow $(LDFLAGS)

C_SRCS := common.c args.c pyxis_slurmstepd.c pyxis_slurmd.c pyxis_srun.c pyxis_alloc.c pyxis_dispatch.c config.c enroot.c
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
	tar -caf ../nvslurm-plugin-pyxis_$(PYXIS_VER).orig.tar.xz --owner=root --group=root --exclude=.git .

deb: clean
	debuild -us -uc -G -i -tc

rpm: clean
	mkdir -p rpm/SOURCES
	tar -caf rpm/SOURCES/nvslurm-plugin-pyxis-$(PYXIS_VER).tar.xz --transform='s,^.,nvslurm-plugin-pyxis-$(PYXIS_VER),' --exclude=rpm --exclude=.git .
	rpmbuild --clean -ba -D"_topdir $(CURDIR)/rpm" -D"PYXIS_VER $(PYXIS_VER)" pyxis.spec
	$(RM) -r $(addprefix rpm/, BUILDROOT BUILD SOURCES)

-include $(DEPS)
