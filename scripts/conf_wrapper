#!/bin/sh

MKFILE=Makefile
CROSS=
CC=gcc
CHECK_DEPS=" \
    make \
    sed \
    xxd \
    gendef \
    shasum \
    git \
" \

CC_CHECK=0
CHECK_CC=" \
    mingw32-$CC \
    i686-w64-mingw32-$CC \
" \

for i in $CHECK_DEPS; do
    echo -n "checking for $i... "
    if [ -z $(which $i 2>/dev/null) ]; then
        echo not found
        exit 1
    fi
    echo $(which $i)
done
if [ ! -z $1 ]; then
    MKFILE=$1
fi
if [ x`uname | sed "s/_.*//"` == xMINGW32 ]; then
    echo "  MSYSTEM=$MSYSTEM"
    TARGET_CC=`gcc -v 2>&1 | grep ^Target`
    echo "  $TARGET_CC"
    if expr "$TARGET_CC" : ".*\(\ i686.*mingw32\)" > /dev/null ||\
        expr "$TARGET_CC" : ".*\(\ mingw32\)" > /dev/null; then
        CC_CHECK=1
    else
        echo "Error: Invalid CC target for WIN32"
        exit 1
    fi
fi
if [ x`uname` == xDarwin ] || [ x`uname` == xLinux ]; then
    echo "  `uname` cross build"
    CC_CHECK=2
fi
if [ $CC_CHECK -eq 0 ]; then
    echo "Error: Unsupported cross build"
    if [ ! -z $MSYSTEM ]; then
        echo "  Invalid MSYSTEM=$MSYSTEM"
    fi
    exit 1
fi
if [ $CC_CHECK -eq 2 ]; then
    for i in $CHECK_CC; do
        if [ ! -z $(which $i 2>/dev/null) ]; then
            CROSS=`which $i | sed "s/.*\///;s/$CC.*//"`
            echo "  CROSS=$CROSS"
            echo "  CC=$CC"
        fi
    done
    if [ -z $CROSS ]; then
        echo "Error: Supported cross build options not found"
        for j in $CHECK_CC; do
            echo "  $j" | sed "s/\-$CC//"
        done
        exit 1
    fi
fi
if [ ! -f ../src/Makefile.in ]; then
    echo "Error: Missing ../src/Makefile.in"
    exit 1
fi
cat ../src/Makefile.in | sed "s/^\(CROSS=\).*/\1$CROSS/" > $MKFILE
if [ $CC_CHECK -eq 2 ]; then
    sed -i -e "s/^\(RC=\)/\1\$(CROSS)/" $MKFILE
    sed -i -e "s/^\(STRIP=\)/\1\$(CROSS)/" $MKFILE
    sed -i -e "s/^\(DLLTOOL=\)/\1\$(CROSS)/" $MKFILE
    sed -i -e "s/^\(TOOLS=\)wglinfo.exe.*/\1/" $MKFILE
    sed -i -e "/.*MSYSTEM\"\ !=\ \"MINGW32\".*/d" $MKFILE
fi
if [ -z $(which wcc386 2>/dev/null) ]; then
    echo -- DOS32 OVL not supported --
    sed -i -e "/.*make\ \-C\ .*ovl/d" $MKFILE
fi
if [ -z $(which i686-pc-msdosdjgpp-gcc 2>/dev/null) ]; then
    echo -- DJGPP DXE not supported --
    sed -i -e "/.*make\ \-C\ .*dxe/d" $MKFILE
fi
echo "conf_wrapper: creating $MKFILE"

