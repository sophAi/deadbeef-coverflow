if HAVE_LIBRETRO
if HAVE_SSE3
SSE3_CFLAGS=-msse3
endif
ppsimdpath=@top_srcdir@/external/ddb_dsp_libretro/ppsimd
EXTRA_DIST = COPYING $(ppsimdpath)/ppsimd-license.txt
pkglib_LTLIBRARIES = ddb_dsp_libretro.la
ddb_dsp_libretro_la_SOURCES = libretro.cpp ppsimd/ppsimd.h sinc_resampler.h
ddb_dsp_libretro_la_LDFLAGS = -module -avoid-version
ddb_dsp_libretro_la_CXXFLAGS = $(CFLAGS) $(SSE3_CFLAGS) -std=c++11 -I@top_srcdir@/include
endif
