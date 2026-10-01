FROM ubuntu:24.04@sha256:008173c23f95b170204355c12626cb5a965d779a7e1283b09e9cffbb1bf33ca3
RUN apt-get update && apt-get install -y --no-install-recommends clang-18 lld-18 llvm-18 make ninja-build python3 wget curl ca-certificates unzip git && rm -rf /var/lib/apt/lists/*
ENV BUILD_JOBS=2 USE_CCACHE=0
WORKDIR /work
CMD ["make", "ps5"]
