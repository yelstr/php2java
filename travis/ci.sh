cd ../../
./buildconf --force

debug=no

while test $# -gt 0; do
  if test "$1" = "--debug"; then
    debug=yes
  fi

  shift
done

if test "$debug" = "yes"; then
  ./configure --prefix=/usr/local/php72 --disable-all --enable-php2java --disable-cgi --disable-cli --enable-debug
else
  ./configure --prefix=/usr/local/php72 --disable-all --enable-php2java --disable-cgi --disable-cli
fi

sleep 5

make php2java