AC_INIT([abbeyd], [1.7], [matthew@ife.onl])

AC_CONFIG_MACRO_DIR([m4])
AM_INIT_AUTOMAKE([foreign -Wall -Wno-extra-portability -Werror subdir-objects])

LT_INIT
AC_PROG_CC
AC_CONFIG_HEADERS([config.h])

PKG_CHECK_MODULES([CURL], libcurl)
PKG_CHECK_MODULES([SQLITE3], sqlite3)
PKG_CHECK_MODULES([JSON], json-c)

AC_SEARCH_LIBS([ev_loop_new], [ev])
AC_SUBST([AM_CPPFLAGS], [-D_GNU_SOURCE])

AC_CONFIG_FILES([Makefile src/Makefile src/ini/Makefile])
AC_OUTPUT
