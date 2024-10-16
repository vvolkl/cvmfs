#!/bin/sh

if [ ! -f arch ]; then
  echo '#include <stdio.h>
    int main(){printf("%d", sizeof(long));return 0;}' | cc -x c -
  SIZEOF_LONG=$(./a.out)
  rm -f a.out
  case $SIZEOF_LONG in
    8)
      echo 'int main() {
#ifdef __x86_64__
return 0;
#endif
return 1;}' | cc -x c -
      ./a.out
      x86_64=$?
      rm -f a.out
      if [ $x86_64 -eq 0 ]; then
        echo 64opt > arch
      else
        echo 64compact > arch
      fi
    ;;
    4)
      echo 32BI > arch
    ;;
  esac
fi

rm -f SnP-interface.h
ln -s $(cat arch)/SnP-interface.h SnP-interface.h

make clean
make CVMFS_CASE_C_FLAGS="$CVMFS_BASE_C_FLAGS" ARCH=$(cat arch) -j  ${CVMFS_BUILD_EXTERNAL_NJOBS}
strip -S libsha3.a

cp -v *.h $EXTERNALS_INSTALL_LOCATION/include/
cp -v libsha3.a $EXTERNALS_INSTALL_LOCATION/lib/
