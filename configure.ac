AC_PREREQ([2.69])
AC_INIT([become-root], [0.1], [giuseppe@scrivano.org])
AC_CONFIG_SRCDIR([main.c])
AC_CONFIG_HEADERS([config.h])


AM_INIT_AUTOMAKE([1.9 foreign])

AC_SEARCH_LIBS([libsubid_init], [subid], [AC_DEFINE([HAVE_LIBSUBID], 1, [Define if libsubid is available])])

AC_PROG_CC

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
