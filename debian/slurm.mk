# Use the installed development package to identify the Slurm provider.
SLURM_DEV := $(shell dpkg-query -W \
    -f='$${db:Status-Status} $${Package}\n' slurm-smd-dev libslurm-dev \
    2>/dev/null | awk '$$1 == "installed" {print $$2; exit}')
ifeq ($(SLURM_DEV),)
$(error Install slurm-smd-dev or libslurm-dev before building)
endif
SLURM_VERSION := $(shell dpkg-query -W -f='$${Version}' $(SLURM_DEV))
SLURM_SERIES := $(shell echo '$(SLURM_VERSION)' | sed 's/.*://' | \
    cut -d. -f1,2)
SLURM_EPOCH := $(shell echo '$(SLURM_VERSION)' | sed -n 's/:.*$$/:/p')
SLURM_NEXT := $(SLURM_EPOCH)$(shell echo '$(SLURM_SERIES)' | \
    awk -F. '{printf "%s.%02d", $$1, $$2 + 1}')
SLURM_PACKAGE := slurm-wlm-basic-plugins
ifeq ($(SLURM_DEV),slurm-smd-dev)
SLURM_PACKAGE := slurm-smd
endif
