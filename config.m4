dnl $Id: config.m4,v 1.2.4.2 2009/03/06 11:12:55 tony Exp $

PHP_ARG_ENABLE(pinba, for Pinba support,
[  --enable-pinba[=DIR]         Include Pinba support.])

PHP_ARG_WITH(pinba-lz4, for Pinba LZ4 compression support,
[  --with-pinba-lz4[=DIR]       liblz4 prefix for compression support.], no, no)

if test "$PHP_PINBA" != "no"; then

  AC_CHECK_HEADERS(malloc.h)
  PHP_CHECK_FUNC(mallinfo)

  if test "$PHP_PINBA_LZ4" != "no"; then
    SEARCH_PATH="/usr /usr/local /local"
    SEARCH_FOR="include/lz4.h"
    if test "$PHP_PINBA_LZ4" = "yes"; then
      AC_MSG_CHECKING([for LZ4 headers in default path])
      for i in $SEARCH_PATH ; do
        if test -r $i/$SEARCH_FOR; then
          LZ4_DIR=$i
          AC_MSG_RESULT(found in $i)
        fi
      done
    else
      AC_MSG_CHECKING([for liblz4 headers in $PHP_PINBA_LZ4])
      if test -r $PHP_PINBA_LZ4/$SEARCH_FOR; then
        LZ4_DIR=$PHP_PINBA_LZ4
        AC_MSG_RESULT([found])
      fi
    fi

    if test -z "$LZ4_DIR"; then
      AC_MSG_RESULT([not found])
      AC_MSG_ERROR([Cannot find liblz4 headers])
    fi

    PHP_ADD_INCLUDE($LZ4_DIR/include)

    LIBNAME=lz4
    LIBSYMBOL=LZ4_compress_default

    PHP_CHECK_LIBRARY($LIBNAME, $LIBSYMBOL,
    [
      PHP_ADD_LIBRARY_WITH_PATH($LIBNAME, $LZ4_DIR/$PHP_LIBDIR, PINBA_SHARED_LIBADD)
    ],[
      AC_MSG_ERROR([wrong liblz4 version or lib not found])
    ],[
      -L$LZ4_DIR/$PHP_LIBDIR
    ])

    AC_DEFINE(HAVE_PINBA_LZ4, 1, [Pinba LZ4 compression support enabled])
    PHP_ADD_LIBRARY(dl, , PINBA_SHARED_LIBADD)
    PHP_SUBST(PINBA_SHARED_LIBADD)
  fi

  PHP_NEW_EXTENSION(pinba, pinba-pb-c.c pinba.c protobuf-c.c, $ext_shared,, -DNDEBUG)
fi
