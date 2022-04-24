#
# Directly taken from fuzz/Dockerfile of this repo
#

FROM ubuntu:focal
ENV DEBIAN_FRONTEND=noninteractive
ENV CC=clang-14
ENV CXX=clang++-14
RUN apt-get update
RUN apt-get install -y cmake git libssl-dev libudev-dev make pkg-config
RUN apt-get install -y libpcsclite-dev zlib1g-dev software-properties-common
RUN git clone --branch v0.9.0 --depth=1 https://github.com/PJK/libcbor
RUN git clone --depth=1 https://github.com/yubico/libfido2
WORKDIR /libfido2
RUN ./.actions/setup_clang "${CC}"
RUN ./fuzz/build-coverage /libcbor /libfido2

#
# mayhem specifics
#

RUN cp /libfido2/build/fuzz/fuzz_bio /fuzzme
