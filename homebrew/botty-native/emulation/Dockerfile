FROM ubuntu:26.04
ENV DEBIAN_FRONTEND=noninteractive EDEN_BUILD_JOBS=3 RADV_BUILD_JOBS=3 CMAKE_BUILD_PARALLEL_LEVEL=3
COPY ubuntu-packages.txt /tmp/packages.txt
RUN apt-get update && sed '/^#/d' /tmp/packages.txt | xargs apt-get install -y --no-install-recommends && rm -rf /var/lib/apt/lists/*
RUN ln -s /usr/bin/llvm-config-21 /usr/local/bin/llvm-config
WORKDIR /work
