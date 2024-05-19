FROM fedora:40

WORKDIR /root

RUN dnf -y install mingw32-gcc mingw-w64-tools curl git gcc-c++ which bison flex texinfo patch zlib-devel tar bzip2 gzip xz unzip python-devel m4 dos2unix nasm diffutils perl-File-Compare perl-Digest-SHA automake autoconf xxd && \
    dnf clean all

ENV WATCOM=/opt/openwatcom
RUN curl -OL https://github.com/open-watcom/open-watcom-v2/releases/download/2024-05-02-Build/open-watcom-2_0-c-linux-x64 && \
    chmod +x open-watcom-2_0-c-linux-x64 && \
    TERM=linux script -qefc "./open-watcom-2_0-c-linux-x64 -is" /dev/null  && \
    rm open-watcom-2_0-c-linux-x64
RUN git clone https://github.com/jwt27/build-gcc.git && \
    cd build-gcc && \
    git checkout e7135d2 && \
    sed -i '/build-gdb.sh/d' build-djgpp.sh && \
    ./build-djgpp.sh --target=i686-pc-msdosdjgpp --prefix=/usr/local --batch all  && \
    cd .. && \
    rm -rf build-gcc
COPY container_build_wrapper /root/container_build_wrapper
CMD /root/container_build_wrapper
