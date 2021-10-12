dnl
dnl $Id$
dnl

PHP_ARG_ENABLE(php2java, for php2java support,
[  --enable-php2java          Enable building of the php2java SAPI executable], no, no)

if test "$PHP_PHP2JAVA" != "no"; then

  PHP_PHP2JAVA_CFLAGS="-D_GNU_SOURCE -DZEND_ENABLE_STATIC_TSRMLS_CACHE=1"
  PHP_PHP2JAVA_FILES="php2java_main.c \
    php2java_parse.c \
    bytebuffer.c
  "
  SAPI_PHP2JAVA_PATH="sapi/php2java/php2java"

  PHP_SUBST(PHP_PHP2JAVA_CFLAGS)
  PHP_SUBST(PHP_PHP2JAVA_FILES)

  PHP_ADD_MAKEFILE_FRAGMENT($abs_srcdir/sapi/php2java/Makefile.frag,)
  PHP_SELECT_SAPI(php2java, program, $PHP_PHP2JAVA_FILES, $PHP_PHP2JAVA_CFLAGS, $SAPI_PHP2JAVA_PATH)


  BUILD_PHP2JAVA="\$(LIBTOOL) --mode=link \
        \$(CC) -export-dynamic \$(CFLAGS_CLEAN) \$(EXTRA_CFLAGS) \$(EXTRA_LDFLAGS_PROGRAM) \$(LDFLAGS) \$(PHP_RPATHS) \
                \$(PHP_GLOBAL_OBJS) \
                \$(PHP_BINARY_OBJS) \
                \$(PHP_PHP2JAVA_OBJS) \
                \$(EXTRA_LIBS) \
                \$(PHP2JAVA_EXTRA_LIBS) \
                \$(ZEND_EXTRA_LIBS) \
         -o \$(SAPI_PHP2JAVA_PATH)"

  PHP_SUBST(SAPI_PHP2JAVA_PATH)
  PHP_SUBST(BUILD_PHP2JAVA)
fi

dnl ## Local Variables:
dnl ## tab-width: 4
dnl ## End:
