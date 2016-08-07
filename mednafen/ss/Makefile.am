AUTOMAKE_OPTIONS = subdir-objects
DEFS = -DLOCALEDIR=\"$(datadir)/locale\" @DEFS@
AM_CXXFLAGS = @AM_CXXFLAGS@ @SS_EXTRA_FLAGS@ @NO_STACK_PROTECTOR_FLAGS@
DEFAULT_INCLUDES = -I$(top_builddir)/include -I$(top_srcdir)/include -I$(top_srcdir)/intl

noinst_LIBRARIES	=

if WANT_SS_EMU
noinst_LIBRARIES	+=	libss.a
endif

if WANT_SSFPLAY_EMU
noinst_LIBRARIES	+=	libssfplay.a
endif

libss_a_SOURCES 	= 	db.cpp cdb.cpp sound.cpp cart.cpp ss.cpp
libss_a_SOURCES		+=	scu_dsp_gen.cpp scu_dsp_mvi.cpp scu_dsp_jmp.cpp scu_dsp_misc.cpp
libss_a_SOURCES		+=	vdp1.cpp vdp1_line.cpp vdp1_sprite.cpp vdp1_poly.cpp
libss_a_SOURCES		+=	vdp2.cpp vdp2_render.cpp
libss_a_SOURCES		+=	smpc.cpp input/gamepad.cpp input/3dpad.cpp input/mouse.cpp

libssfplay_a_SOURCES	=	ssf.cpp
