dnl $Id: config.m4,v 1.2.4.2 2009/03/06 11:12:55 tony Exp $

PHP_ARG_ENABLE(pinba, for Pinba support,
[  --enable-pinba[=DIR]         Include Pinba support.])

if test "$PHP_PINBA" != "no"; then
  PHP_NEW_EXTENSION(pinba, pinba-pb-c.c pinba.c protobuf-c.c, $ext_shared,, -DNDEBUG)
fi
