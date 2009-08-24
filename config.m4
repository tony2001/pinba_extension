dnl $Id: config.m4,v 1.2.4.2 2009/03/06 11:12:55 tony Exp $

PHP_ARG_WITH(pinba, for Pinba support,
[  --with-pinba[=DIR]           Include Pinba support.
                                DIR is Google Protocol Buffers install prefix.])

if test "$PHP_PINBA" != "no"; then
  SEARCH_PATH="/usr /usr/local /opt"
  SEARCH_FOR="/include/google/protobuf/descriptor.h"

  if test -d "$PHP_PINBA"; then
    AC_MSG_CHECKING([for Google Protocol Buffers files in $PHP_PINBA])
    if test -r "$PHP_PINBA/$SEARCH_FOR"; then 
      PROTOBUF_DIR=$PHP_PINBA
      AC_MSG_RESULT([found])
    fi
  else
    AC_MSG_CHECKING([for Google Protocol Buffers files in default path])
    for i in $SEARCH_PATH ; do
      if test -r $i/$SEARCH_FOR; then
        PROTOBUF_DIR=$i
        AC_MSG_RESULT(found in $i)
      fi
    done
  fi

  if test -z "$PROTOBUF_DIR"; then
    AC_MSG_RESULT([not found])
    AC_MSG_ERROR([Unable to find Google Protocol Buffers headers])
  fi

  PHP_ADD_INCLUDE($PROTOBUF_DIR/include)

  AC_MSG_CHECKING([for Google Protobuf compiler])
  PROTOC="x"
  if ! test -x "$PROTOBUF_DIR/bin/protoc"; then
    AC_MSG_ERROR([Unable to find Protobuf compiler (protoc) in $PROTOBUF_DIR/bin])
  else
    PROTOC="$PROTOBUF_DIR/bin/protoc"
    AC_MSG_RESULT([$PROTOC])
  fi

  AC_MSG_CHECKING([for Google Protobuf version])
  gpb_full_version=`$PROTOC --version | $SED -e 's/libprotoc //'`
  ac_IFS=$IFS
  IFS="."
  set $gpb_full_version
  IFS=$ac_IFS
  GPB_VERSION=`expr [$]1 \* 1000000 + [$]2 \* 1000 + [$]3`
  AC_MSG_RESULT([$gpb_full_version])

  dnl Google Protobuf >= 2.1.0 uses pthread_once() in its headers
  if test "$GPB_VERSION" -ge "2001000"; then
    old_LDFLAGS=$LDFLAGS
    LDFLAGS=-lpthread
    THREAD_LIB=""
    AC_CHECK_FUNC(pthread_once,[
      THREAD_LIB=pthread
    ],[
      LDFLAGS=-lc_r
      AC_CHECK_FUNC(pthread_once, [
        THREAD_LIB=c_r
      ],[
        LDFLAGS=-lc
        AC_CHECK_FUNC(pthread_once, [
          THREAD_LIB=c
        ],[
          AC_MSG_ERROR([pthread_once() is required for Google Protocol Buffers >= 2.1.0, but it could not be found, exiting])
        ])
      ])
    ])
    PHP_ADD_LIBRARY([$THREAD_LIB])
  fi

  PHP_REQUIRE_CXX
  PHP_ADD_LIBRARY_WITH_PATH(stdc++, "", PINBA_SHARED_LIBADD)
  PHP_ADD_LIBRARY_WITH_PATH(protobuf, $PROTOBUF_DIR/lib, PINBA_SHARED_LIBADD)
  PHP_SUBST(PINBA_SHARED_LIBADD)
  PHP_NEW_EXTENSION(pinba, pinba.cc pinba-pb.cc, $ext_shared,, -DNDEBUG)

  AC_MSG_NOTICE([Regenerating protocol code])
  `$PROTOC -I$ext_srcdir $ext_srcdir/pinba.proto --cpp_out=$ext_srcdir`

  if test "$?" != 0; then
    AC_MSG_ERROR([Failed to regenerate protocol code])
  fi

  `$SED -e 's/pinba\.pb\.h/pinba-pb.h/' $ext_srcdir/pinba.pb.cc > $ext_srcdir/pinba-pb.cc && rm $ext_srcdir/pinba.pb.cc`
  if test "$?" != 0; then
    AC_MSG_ERROR([Failed to run sed])
  fi

  `mv $ext_srcdir/pinba.pb.h $ext_srcdir/pinba-pb.h`
  if test "$?" != 0; then
    AC_MSG_ERROR([Failed to rename pinba.pb.h to pinba-pb.h])
  fi

fi
