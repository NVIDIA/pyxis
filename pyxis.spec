Name:           nvslurm-plugin-pyxis
Version:        %{VERSION}
Release:        1%{?dist}
License:        ASL 2.0
Vendor: NVIDIA CORPORATION
Packager: NVIDIA CORPORATION <cudatools@nvidia.com>
URL:            https://github.com/NVIDIA/pyxis/

BuildRequires:  make gcc slurm-devel

Summary:        Pyxis is a SPANK plugin for the SLURM workload manager
Requires:       (enroot >= 3.1.0 or enroot-hardened >= 3.1.0)

%description
Pyxis is a SPANK plugin for the SLURM Workload Manager. It allows unprivileged
cluster users to run containerized tasks through the srun command.

%files
%license LICENSE
%doc README.md
%{_libdir}/slurm/*
%{_datadir}/pyxis/pyxis.conf

%prep

%build
%make_build prefix=%{_prefix}

%install
%make_install prefix=%{_prefix} libdir=%{_libdir} DESTDIR=%{buildroot}


%changelog
* Thu May 05 2022 NVIDIA CORPORATION <cudatools@nvidia.com> - 0.13.0-1
- Release v0.13.0

* Fri Jan 28 2022 NVIDIA CORPORATION <cudatools@nvidia.com> - 0.12.0-1
- Release v0.12.0

* Mon Jun 14 2021 NVIDIA CORPORATION <cudatools@nvidia.com> - 0.11.0-1
- Release v0.11.0

* Mon May 10 2021 NVIDIA CORPORATION <cudatools@nvidia.com> - 0.10.0-1
- Release v0.10.0

* Thu Dec 03 2020 NVIDIA CORPORATION <cudatools@nvidia.com> - 0.9.1-1
- Release v0.9.1

* Tue Dec 01 2020 NVIDIA CORPORATION <cudatools@nvidia.com> - 0.9.0-1
- Release v0.9.0

* Thu Aug 20 2020 NVIDIA CORPORATION <cudatools@nvidia.com> - 0.8.1-1
- Release v0.8.1

* Tue Aug 04 2020 NVIDIA CORPORATION <cudatools@nvidia.com> - 0.8.0-1
- Release v0.8.0

* Wed Jul 01 2020 NVIDIA CORPORATION <cudatools@nvidia.com> - 0.7.0-1
- Release v0.7.0

* Thu Mar 26 2020 NVIDIA CORPORATION <cudatools@nvidia.com> - 0.6.0-1
- Release v0.6.0

* Thu Mar 12 2020 NVIDIA CORPORATION <cudatools@nvidia.com> - 0.5.0-1
- Release v0.5.0

* Tue Dec 10 2019 NVIDIA CORPORATION <cudatools@nvidia.com> - 0.4.0-1
- Release v0.4.0

* Mon Oct 28 2019 NVIDIA CORPORATION <cudatools@nvidia.com> - 0.3.0-1
- Release v0.3.0

* Thu Sep 05 2019 NVIDIA CORPORATION <cudatools@nvidia.com> - 0.2.0-1
- Release v0.2.0

* Thu Jul 25 2019 NVIDIA CORPORATION <cudatools@nvidia.com> - 0.1.0-1
- Initial release
