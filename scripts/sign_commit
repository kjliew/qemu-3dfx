#!/bin/sh

TOP=.
GIT=`echo $@ | sed "s/\-git=\(.*\)/\1/;s/\ .*//"`
if [ -z $GIT ]; then GIT=.; fi; shift
ARG=$@; if [ -z $ARG ]; then ARG=HEAD; fi
REV=`cd $GIT; git rev-parse $ARG | sed "s/\(.......\).*/\1\-/"`
if [ -z $REV ]; then exit 1; fi
VLC=$(find $TOP -maxdepth 2 -name vl.c)
SRC=" \
    $TOP/hw/3dfx/g2xfuncs.h \
    $TOP/hw/mesa/mglfuncs.h \
    $VLC \
" \

echo $REV
sed -i -e "s/\(rev_\[\).*\].*/\1\]\ =\ \"$REV\"/" $SRC
grep "rev_\[" $SRC | sed "s/\(rev_\[\).*\].*/\1\]\ =\ \"$REV\"/"
