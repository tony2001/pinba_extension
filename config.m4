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
    AC_MSG_ERROR([Unable to find Protobuf compiler (protoc)])
  else
    PROTOC="$PROTOBUF_DIR/bin/protoc"
    AC_MSG_RESULT([$PROTOC])
  fi

  PHP_REQUIRE_CXX
  PHP_ADD_LIBRARY_WITH_PATH(stdc++, "", PINBA_SHARED_LIBADD)
  PHP_ADD_LIBRARY_WITH_PATH(protobuf, $PROTOBUF_DIR/lib, PINBA_SHARED_LIBADD)
  PHP_SUBST(PINBA_SHARED_LIBADD)
  PHP_NEW_EXTENSION(pinba, pinba.cc pinba-pb.cc, $ext_shared)

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
