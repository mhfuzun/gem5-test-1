FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt update && apt install -y \
    build-essential \
    scons \
    python3 \
    python3-dev \
    python3-pip \
    git \
    gcc-riscv64-linux-gnu \
    g++-riscv64-linux-gnu \
    make \
    pkg-config \
    libprotobuf-dev \
    protobuf-compiler \
    libboost-all-dev \
    zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
