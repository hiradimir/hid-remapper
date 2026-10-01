FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    gcc-arm-none-eabi \
    libnewlib-arm-none-eabi \
    libstdc++-arm-none-eabi-newlib \
    git \
    curl \
    python3 \
    python3-pip \
    ninja-build \
    gdb-multiarch

RUN ARCH=$(uname -m) && \
    if [ "$ARCH" = "x86_64" ]; then OPENOCD_ARCH="x64"; else OPENOCD_ARCH="arm64"; fi && \
    curl -L "https://github.com/xpack-dev-tools/openocd-xpack/releases/download/v0.12.0-4/xpack-openocd-0.12.0-4-linux-${OPENOCD_ARCH}.tar.gz" | tar -xz -C /opt && \
    ln -sf /opt/xpack-openocd-0.12.0-4/bin/openocd /usr/bin/openocd


RUN ln -sf /usr/bin/gdb-multiarch /usr/bin/arm-none-eabi-gdb \
    && ln -sf /usr/bin/arm-none-eabi-nm /usr/bin/nm-multiarch \
    && ln -sf /usr/bin/arm-none-eabi-objdump /usr/bin/objdump-multiarch

RUN rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

