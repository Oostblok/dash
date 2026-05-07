# Force the image to be x86/amd64 so the compiler doesn't segfault
FROM --platform=linux/amd64 ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# 1. Enable ARM64 architecture support (for the libraries)
RUN dpkg --add-architecture arm64

# 2. Install Cross-Compiler + ARM64 Librariesz
RUN apt-get update && apt-get install -y \
    cmake build-essential git pkg-config wget \
    crossbuild-essential-arm64 \
    protobuf-compiler \
    libprotobuf-dev \
    # The :arm64 libraries stay the same
    libboost-all-dev:arm64 \
    libusb-1.0-0-dev:arm64 \
    libssl-dev:arm64 \
    libprotobuf-dev:arm64 \
    libqt5multimedia5:arm64 \
    qtmultimedia5-dev:arm64 \
    libqt5bluetooth5:arm64 \
    qtconnectivity5-dev:arm64 \
    librtaudio-dev:arm64 \
    libtag1-dev:arm64 \
    libgstreamer1.0-dev:arm64 \
    libgstreamer-plugins-base1.0-dev:arm64 \
    libgstreamer-plugins-bad1.0-dev:arm64 \
    qtdeclarative5-dev:arm64 \
    libqt5serialbus5-dev:arm64 \
    libqt5serialport5-dev:arm64 \
    libqt5websockets5-dev:arm64 \
    libqt5svg5-dev:arm64 \
    libunwind-dev:arm64 \
    libglib2.0-dev:arm64 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /home/rudi/dash
