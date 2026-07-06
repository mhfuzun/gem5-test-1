# Copyright (c) 2026
#
# Builds a gem5 runtime bundle intended to run on older RHEL 7 hosts by
# packaging gem5 together with a private loader, glibc, libstdc++, Python,
# and other shared-library dependencies from a self-contained Ubuntu userspace.
#
# The resulting bundle does not depend on the target host's glibc or Python.

FROM ubuntu:22.04

LABEL org.opencontainers.image.source=https://github.com/gem5/gem5
LABEL org.opencontainers.image.licenses=BSD-3-Clause

ENV DEBIAN_FRONTEND=noninteractive
ENV LANG=C.UTF-8
ENV LC_ALL=C.UTF-8
ENV PYTHON_CONFIG=python3-config

RUN apt-get update && \
    apt-get install -y \
        build-essential \
        python3 \
        python3-dev \
        python3-pip \
        python3-venv \
        git \
        make \
        file \
        findutils \
        diffutils \
        rsync \
        zip \
        unzip \
        zlib1g-dev \
        libprotobuf-dev \
        protobuf-compiler \
        libhdf5-dev \
        libpng-dev \
        pkg-config \
        m4 \
        ca-certificates \
        binutils && \
    python3 -m pip install --no-cache-dir 'scons>=4.0,<5.0' && \
    rm -rf /var/lib/apt/lists/*

COPY collect-runtime.sh /usr/local/bin/collect-runtime.sh
COPY run-gem5.sh /usr/local/bin/run-gem5.sh

RUN chmod +x /usr/local/bin/collect-runtime.sh /usr/local/bin/run-gem5.sh

WORKDIR /src/gem5
