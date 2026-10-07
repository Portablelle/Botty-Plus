# Plus CI runners on dedie

Every workflow uses `[self-hosted, linux, x64, <botty|portal>-plus-ci]`, with
no GitHub-hosted fallback. Unavailable runners leave jobs queued.

## Isolation and registration

Like Ciaobella, each job gets a new single-use GitHub JIT runner identity and
one fresh container in the existing `gh-runner` rootless Docker daemon.
GitHub removes that identity after one job. There are no persistent runner
credentials, host mounts, deployment keys or Docker sockets inside the job.

A root-owned `/usr/local/sbin/plus-runner-api` broker creates/deletes JIT
runners only for `Portablelle/Botty-Plus` and `Portablelle/Portal-Plus`.
It uses Ubuntu's existing authenticated `/snap/bin/gh` installation. The
host `gh-runner` user gets sudo access only to that argument-validated broker;
the underlying administrative token remains in Ubuntu's existing configuration,
never in the runner account, its environment files, or the job container.
Installing this broker and its sudoers rule requires an administrator. Review
that source before installation. Losing/expiring Ubuntu's GitHub login stops
new registrations; re-authenticate Ubuntu to recover it.

Each slot has its own Docker bridge network, with no published ports and no
connection to Ciaobella's Docker network. Public PR code retains outbound
Internet access to fetch dependencies.

The job runs as UID 1001 with no capabilities and no privilege escalation.
The root filesystem is read-only. Writable data is restricted to tmpfs:
`/home/runner` (4 GiB, including a fresh copy of the runner and Node cache),
`/tmp` (512 MiB), and Docker's default `/dev/shm` (64 MiB). Those mounts count
against the 8 GiB memory limit; swap is disabled for the job. CPU is capped at
4, processes at 4096. Docker logs rotate at 10 MiB, keeping two files, so PR
output cannot grow the host's Docker graph without bound.

The immutable image includes Clang, libcurl, zlib development headers,
Python/Pillow, and Node 24. `AGENT_TOOLSDIRECTORY` and `RUNNER_TOOL_CACHE`
point to the fresh writable copy of the preinstalled Node tool cache.

## Install and maintenance

On this server the host accounts `ubuntu` and `gh-runner` already exist.
`gh-runner` has a rootless Docker daemon at `/run/user/1001/docker.sock`.
Ubuntu has an existing GitHub login with repository administration access
for both Plus repos. This installation uses that existing login without
creating or copying an administrative credential. On another host adapt
those account/path prerequisites first; do not grant arbitrary sudo or
membership in the system Docker group to `gh-runner`.

Host prerequisites are `curl`, `jq`, Python 3, sudo and rootless Docker.
`sudo apt-get install -y curl jq` installs the build tools on Ubuntu/Debian;
install-host.sh installs these prerequisites and verifies the existing accounts.

Run `bash infra/ci-runner/install-host.sh` from a reviewed checkout **on dedie**.
It installs the root-owned broker and restricted sudoers rule, installs the
user units, builds the image, and enables both slots and the weekly timer.
It also runs `sudo loginctl enable-linger gh-runner`, so these user services
and the timer run after logout and reboot.

The weekly image timer validates version discovery and official download
hashes before building. Only superseded Plus runner images are cleaned up;
images still used by jobs and unrelated Ciaobella images are retained.
The next newly created JIT container uses the new image. An already
listening container, including an idle one, keeps its old image until it exits.
The weekly rebuild does not restart containers or interrupt running tests.
The legacy Docker builder on this dedicated server also limits build
containers to 2 CPUs and 4 GiB; the image service gives its CLI low CPU
weight and an 8 GiB memory ceiling. Failed image builds retry after 60 seconds, at most five starts per ten
minutes, so a temporarily unavailable network at boot is retried.

`plus-runner@botty` and `plus-runner@portal` use `Wants=docker.service` and
restart after Docker failures. Each normal exit revokes any unused JIT
identity. `ExecStopPost=stop-slot.sh` also stops/removes the container and
revokes the identity after an unexpected/forced slot exit. Cleanup has bounded
timeouts, and `KillMode=mixed` cleans up remaining unit processes.
A manual service stop/restart intentionally aborts an in-flight CI job after
up to 20 seconds; do this only when a job may be cancelled. Weekly image
builds do not stop/restart the slots. Registration failures emit a distinct
`JIT_REGISTRATION_FAILED` marker and retry with exponential backoff capped
at five minutes, resetting after a completed runner session.

Inspect logs as gh-runner with `XDG_RUNTIME_DIR=/run/user/1001`:
`journalctl --user -u plus-runner@botty -u plus-runner@portal`.
Never commit GitHub tokens, JIT configurations, or runner credential files.
