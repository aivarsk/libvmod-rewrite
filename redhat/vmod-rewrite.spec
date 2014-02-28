%define VMODNAME   libvmod-rewrite
%define VMODSRC    %{_builddir}/%{VMODNAME}
%define VARNISHSRC ../SOURCES/varnish-3.0.4

Summary: Rewrite VMOD for Varnish
Name: varnish-vmod-rewrite
Version: 0.1
Release: 1%{?dist}
License: BSD
Group: System Environment/Daemons
Source0: libvmod-rewrite.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)
Requires: varnish > 3.0
BuildRequires: make, python-docutils

%description
Varnish vmod hack demonstrating how to rewrite HTML content. It's not production-ready - I'm still learning and looking for the best way how to do it. https://github.com/pbruna/libvmod-rewrite

%prep
%setup -n libvmod-rewrite

%build
# this assumes that VARNISHSRC is defined on the rpmbuild command line, like this:
# rpmbuild -bb --define 'VARNISHSRC /home/user/rpmbuild/BUILD/varnish-3.0.3' redhat/*spec


cd %{VARNISHSRC}
./autogen.sh
%{configure}
make %{?_smp_mflags}

cd %{VMODSRC}
VMODDIR="$(PKG_CONFIG_PATH=%{VARNISHSRC} pkg-config --variable=vmoddir varnishapi)"
./autogen.sh
./configure VARNISHSRC=%{VARNISHSRC} VMODDIR="$(PKG_CONFIG_PATH=%{VARNISHSRC} pkg-config --variable=vmoddir varnishapi)" --prefix=/usr/ --docdir='${datarootdir}/doc/%{name}'

make
make check

%install
make install DESTDIR=%{buildroot}

%clean
rm -rf %{buildroot}

%files
%defattr(-,root,root,-)
%{_libdir}/varnish/vmods/
%{_mandir}/man?/*

%changelog
* Fri Feb 28 2014 Patricio Bruna V. <pbruna@itlinux.cl> - 0.1-1
- Initial version.