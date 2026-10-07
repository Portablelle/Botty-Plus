# The image every job of the dedie runners runs in: one fresh container per job (see slot.sh).
FROM ubuntu:24.04

LABEL com.portablelle.plus-runner=true

ARG RUNNER_VERSION
ARG NODE_VERSION
ARG RUNNER_SHA256
ARG NODE_SHA256
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
  && apt-get install -y --no-install-recommends \
    build-essential ca-certificates curl git gnupg jq openssh-client python3 python3-pil clang libcurl4-openssl-dev unzip xz-utils zstd \
  && curl -fsSL https://cli.github.com/packages/githubcli-archive-keyring.gpg -o /usr/share/keyrings/githubcli.gpg \
  && echo "deb [signed-by=/usr/share/keyrings/githubcli.gpg] https://cli.github.com/packages stable main" \
    > /etc/apt/sources.list.d/github-cli.list \
  && apt-get update && apt-get install -y --no-install-recommends gh \
  && rm -rf /var/lib/apt/lists/*

# actions/checkout runs `git init`: name its default branch so git prints no hint in every job.
RUN git config --system init.defaultBranch main

RUN useradd --create-home --uid 1001 runner
WORKDIR /home/runner

RUN runner_archive="$(mktemp)" \
  && curl -fsSL --retry 3 --retry-all-errors --connect-timeout 15 --max-time 120 \
    "https://github.com/actions/runner/releases/download/v${RUNNER_VERSION}/actions-runner-linux-x64-${RUNNER_VERSION}.tar.gz" \
    -o "$runner_archive" \
  && echo "${RUNNER_SHA256}  $runner_archive" | sha256sum --check --status - \
  && tar xzf "$runner_archive" \
  && rm -f "$runner_archive" \
  && ./bin/installdependencies.sh \
  && rm -rf /var/lib/apt/lists/* \
  && chown -R runner:runner .

# Node 24 where setup-node looks first, so jobs don't download it each time.
ENV AGENT_TOOLSDIRECTORY=/opt/hostedtoolcache
ENV RUNNER_TOOL_CACHE=/opt/hostedtoolcache
RUN node_archive="$(mktemp)" \
  && mkdir -p "/opt/hostedtoolcache/node/${NODE_VERSION}/x64" \
  && curl -fsSL --retry 3 --retry-all-errors --connect-timeout 15 --max-time 120 \
    "https://nodejs.org/dist/v${NODE_VERSION}/node-v${NODE_VERSION}-linux-x64.tar.xz" \
    -o "$node_archive" \
  && echo "${NODE_SHA256}  $node_archive" | sha256sum --check --status - \
  && tar xJf "$node_archive" --strip-components=1 -C "/opt/hostedtoolcache/node/${NODE_VERSION}/x64" \
  && rm -f "$node_archive" \
  && touch "/opt/hostedtoolcache/node/${NODE_VERSION}/x64.complete" \
  && chown -R runner:runner /opt/hostedtoolcache

# Native Botty tests also link zlib.
RUN apt-get update && apt-get install -y --no-install-recommends zlib1g-dev \
  && rm -rf /var/lib/apt/lists/*

# Keep the immutable runner separate from the size-capped job home.
RUN mv /home/runner /opt/actions-runner && mkdir /home/runner \
  && chown runner:runner /home/runner

USER runner
# slot.sh supplies the JIT configuration and bootstraps the bounded writable home.
CMD ["bash"]
