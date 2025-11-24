FROM ubuntu:noble

# Allow specifying the default compiler: gcc or llvm
ARG COMPILER=llvm
# PVXS version to build
ARG PVXS_VERSION=1.4.1

# Install common dependencies and compiler-specific packages
RUN apt update && \
    apt install -y build-essential cmake git tclsh ninja-build protobuf-compiler protobuf-compiler-grpc libgrpc++-dev libgrpc-dev gdb lcov libssl-dev libsasl2-dev curl libcurl4-openssl-dev valgrind libicu-dev jq file sudo clang clangd lldb lld libc++-dev libc++abi-dev libevent-dev

# Set default compiler based on the COMPILER argument BEFORE building EPICS
RUN if [ "$COMPILER" = "llvm" ]; then \
        update-alternatives --install /usr/bin/cc cc /usr/bin/clang 100 && \
        update-alternatives --install /usr/bin/c++ c++ /usr/bin/clang++ 100 && \
        update-alternatives --set cc /usr/bin/clang && \
        update-alternatives --set c++ /usr/bin/clang++; \
    else \
        update-alternatives --install /usr/bin/cc cc /usr/bin/gcc 100 && \
        update-alternatives --install /usr/bin/c++ c++ /usr/bin/g++ 100 && \
        update-alternatives --set cc /usr/bin/gcc && \
        update-alternatives --set c++ /usr/bin/g++; \
    fi

# Verify the compiler selection
RUN echo "Building with:" && cc --version

RUN git clone --branch R7.0.8.1 --depth 1 --recursive https://github.com/epics-base/epics-base.git /opt/epics
WORKDIR /opt/epics
# Configure EPICS to use the selected compiler
RUN if [ "$COMPILER" = "llvm" ]; then \
        echo "GNU_DIR=" > configure/os/CONFIG_SITE.Common.linux-aarch64 && \
        echo "CC = clang" >> configure/os/CONFIG_SITE.Common.linux-aarch64 && \
        echo "CCC = clang++" >> configure/os/CONFIG_SITE.Common.linux-aarch64 && \
        echo "CMPLR_CLASS = clang" >> configure/os/CONFIG_SITE.Common.linux-aarch64 && \
        echo "GNU_DIR=" > configure/os/CONFIG_SITE.Common.linux-x86_64 && \
        echo "CC = clang" >> configure/os/CONFIG_SITE.Common.linux-x86_64 && \
        echo "CCC = clang++" >> configure/os/CONFIG_SITE.Common.linux-x86_64 && \
        echo "CMPLR_CLASS = clang" >> configure/os/CONFIG_SITE.Common.linux-x86_64 && \
        mkdir -p include/compiler/clang; \
    fi
RUN make -j 4 INSTALL_LOCATION=/opt/local && \
    make install INSTALL_LOCATION=/opt/local

# Set EPICS environment variables
ENV EPICS_BASE=/opt/local

# Determine EPICS host architecture and ensure toolchain paths exist
RUN EPICS_ARCH=$(/opt/epics/startup/EpicsHostArch) && \
    if [ -z "$EPICS_ARCH" ]; then \
        arch=$(uname -m) && \
        case "$arch" in \
            x86_64) EPICS_ARCH="linux-x86_64" ;; \
            aarch64|arm64) EPICS_ARCH="linux-aarch64" ;; \
            *) EPICS_ARCH="linux-$arch" ;; \
        esac; \
    fi && \
    echo "$EPICS_ARCH" > /etc/epics_host_arch && \
    printf 'export EPICS_HOST_ARCH=%s\n' "$EPICS_ARCH" >> /etc/profile.d/epics.sh && \
    printf 'export EPICS_HOST_ARCH=%s\n' "$EPICS_ARCH" >> /etc/bash.bashrc && \
    printf 'EPICS_HOST_ARCH=%s\n' "$EPICS_ARCH" >> /etc/environment && \
    if [ ! -d /opt/epics/bin/${EPICS_ARCH} ] && [ -d /opt/local/bin/${EPICS_ARCH} ]; then \
        mkdir -p /opt/epics/bin && \
        ln -s /opt/local/bin/${EPICS_ARCH} /opt/epics/bin/${EPICS_ARCH}; \
    fi && \
    if [ ! -d /opt/epics/lib/${EPICS_ARCH} ] && [ -d /opt/local/lib/${EPICS_ARCH} ]; then \
        mkdir -p /opt/epics/lib && \
        ln -s /opt/local/lib/${EPICS_ARCH} /opt/epics/lib/${EPICS_ARCH}; \
    fi

# Build PVXS
RUN git clone --branch ${PVXS_VERSION} --depth 1 https://github.com/epics-base/pvxs.git /opt/pvxs
WORKDIR /opt/pvxs
RUN arch=$(cat /etc/epics_host_arch) && \
    echo "EPICS_BASE=/opt/local" > configure/RELEASE.local && \
    echo "EPICS_HOST_ARCH=$arch" >> configure/RELEASE.local && \
    echo "INSTALL_LOCATION=/opt/local" >> configure/RELEASE.local && \
    EPICS_HOST_ARCH=$arch make -j 4 && \
    EPICS_HOST_ARCH=$arch make install && \
    if ! ls /opt/local/lib/${arch}/libpvxs* >/dev/null 2>&1; then \
        mkdir -p /opt/local/lib/${arch} && \
        cp -a /opt/pvxs/lib/${arch}/. /opt/local/lib/${arch}/; \
    fi && \
    if ! ls /opt/local/include/pvxs/version.h >/dev/null 2>&1; then \
        mkdir -p /opt/local/include && \
        cp -a /opt/pvxs/include/. /opt/local/include/; \
    fi

# Set PVXS environment variable
ENV PVXS_BASE=/opt/local

# Download dp-grpc proto files
RUN git clone https://github.com/osprey-dcs/dp-grpc.git /opt/dp-grpc

# Set PROTO_PATH environment variable to the proto files location
ENV PROTO_PATH=/opt/dp-grpc/src/main/proto

# Set the working directory
WORKDIR /workspace

# Define a default command to run when the container starts (optional)
# CMD ["bash"]
