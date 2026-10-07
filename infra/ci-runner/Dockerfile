# The image every job of the dedie runners runs in: one fresh container per job (see slot.sh).
FROM ubuntu:24.04

ARG RUNNER_VERSION
ARG NODE_VERSION
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

RUN curl -fsSL "https://github.com/actions/runner/releases/download/v${RUNNER_VERSION}/actions-runner-linux-x64-${RUNNER_VERSION}.tar.gz" \
    | tar xz \
  && ./bin/installdependencies.sh \
  && rm -rf /var/lib/apt/lists/* \
  && chown -R runner:runner .

# Node 24 where setup-node looks first, so jobs don't download it each time.
ENV AGENT_TOOLSDIRECTORY=/opt/hostedtoolcache
RUN mkdir -p "/opt/hostedtoolcache/node/${NODE_VERSION}/x64" \
  && curl -fsSL "https://nodejs.org/dist/v${NODE_VERSION}/node-v${NODE_VERSION}-linux-x64.tar.xz" \
    | tar xJ --strip-components=1 -C "/opt/hostedtoolcache/node/${NODE_VERSION}/x64" \
  && touch "/opt/hostedtoolcache/node/${NODE_VERSION}/x64.complete" \
  && chown -R runner:runner /opt/hostedtoolcache

# Native Botty tests also link zlib.
RUN apt-get update && apt-get install -y --no-install-recommends zlib1g-dev \
  && rm -rf /var/lib/apt/lists/*

USER runner
# slot.sh copies the repository-scoped identity, then runs as runner with --once.
ENTRYPOINT ["./run.sh"]
