INCLUDES = -I$(VARNISHSRC)/include -I$(VARNISHSRC)

vmoddir = $(VMODDIR)
vmod_LTLIBRARIES = libvmod_rewrite.la

libvmod_rewrite_la_LDFLAGS = -module -export-dynamic -avoid-version

libvmod_rewrite_la_SOURCES = \
	vcc_if.c \
	vcc_if.h \
	vmod_rewrite.c

vcc_if.c vcc_if.h: $(VARNISHSRC)/lib/libvmod_std/vmod.py $(top_srcdir)/src/vmod_rewrite.vcc
	@PYTHON@ $(VARNISHSRC)/lib/libvmod_std/vmod.py $(top_srcdir)/src/vmod_rewrite.vcc

EXTRA_DIST = vmod_rewrite.vcc

CLEANFILES = $(builddir)/vcc_if.c $(builddir)/vcc_if.h
