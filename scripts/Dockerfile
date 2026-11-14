# Compilers with the ALSA headers for the SDK front end: native x86 (host_sim test), armhf glibc 2.31 (Force), arm64 (Live III).
#   docker build -q -t maze-sdk-x64 --build-arg BASE=gcc:12 .
#   docker build -q -t maze-sdk-arm32 --platform linux/arm/v7 --build-arg BASE=arm32v7/gcc:11-bullseye .
#   docker build -q -t maze-sdk-arm64 --platform linux/arm64 --build-arg BASE=arm64v8/gcc:12-bookworm .
ARG BASE=gcc:12
FROM ${BASE}
RUN sed -i "/security/d;/updates/d" /etc/apt/sources.list 2>/dev/null; apt-get update -qq && apt-get install -y -qq -t bullseye libasound2-dev 2>/dev/null || apt-get install -y -qq libasound2-dev; rm -rf /var/lib/apt/lists/*
