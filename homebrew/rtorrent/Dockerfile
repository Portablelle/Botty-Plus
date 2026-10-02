FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends clang-18 lld-18 llvm-18 make autoconf automake libtool pkg-config cmake ninja-build python3 curl ca-certificates xz-utils && rm -rf /var/lib/apt/lists/*
RUN curl -fL https://github.com/ps5-payload-dev/pacbrew-repo/releases/download/v0.40.2/ps5-payload-dev.tar.gz -o /tmp/sdk.tar.gz && echo 'a85f65de418a8e6a898c6c3e3c870d50fff7618a200e4dd59ea9692af6ecec4d  /tmp/sdk.tar.gz' | sha256sum -c - && tar xzf /tmp/sdk.tar.gz -C / && rm /tmp/sdk.tar.gz
# PacBrew provides ports; overlay the separately versioned, pinned payload runtime.
RUN curl -fL https://github.com/ps5-payload-dev/sdk/releases/download/v0.43/ps5-payload-sdk.zip -o /tmp/runtime.zip && echo 'a9cc9929f21b2b2c5d5b309f3bab4997067c45281c0622cf4838b1aecba66fcb  /tmp/runtime.zip' | sha256sum -c - && python3 -m zipfile -e /tmp/runtime.zip /opt && rm /tmp/runtime.zip
ENV LLVM_CONFIG=/usr/bin/llvm-config-18
ENV PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
WORKDIR /work
CMD ["bash", "/work/build.sh"]
