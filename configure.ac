#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.68])
AC_INIT([measurement-testbed], [0.1], [salsify@anon-submission])
AM_INIT_AUTOMAKE([foreign])
AC_CONFIG_SRCDIR([src/display/display.hh])
AC_CONFIG_HEADERS([config.h])

# Add picky CXXFLAGS
CXX14_FLAGS="-std=c++14 -pthread"
PICKY_CXXFLAGS="-pedantic -Wall -Wextra -Weffc++ -Werror -Wno-non-virtual-dtor"
AC_SUBST([CXX14_FLAGS])
AC_SUBST([PICKY_CXXFLAGS])

# Checks for programs.
AC_PROG_CXX
AC_PROG_RANLIB

# Checks for libraries.
PKG_CHECK_MODULES([XCB], [xcb])
PKG_CHECK_MODULES([XCBPRESENT], [xcb-present])

# Checks for header files.

# Checks for typedefs, structures, and compiler characteristics.

# Checks for library functions.

AC_CONFIG_FILES([Makefile
         third_party/Makefile
         third_party/decklink/Makefile
         third_party/daala_tools/Makefile
         src/Makefile
         src/util/Makefile
         src/display/Makefile
         src/playback/Makefile
         src/rgb-example/Makefile
         src/capture/Makefile
         src/barcoder/Makefile
         src/scanner/Makefile
	      src/tests/Makefile
	 ])
AC_OUTPUT
