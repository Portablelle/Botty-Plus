# Plus CI runners on dedie

All workflow jobs use `[self-hosted, linux, x64, botty-plus-ci]`.
There is no GitHub-hosted fallback: if the runner is unavailable, jobs queue.

The dedicated server runs two `plus-runner@` user services (`botty`, `portal`)
as `gh-runner`, using the existing rootless Docker daemon, like Ciaobella.
Every job starts in a fresh Ubuntu 24.04 container (4 CPUs, 8 GiB, 4096 PIDs).
No host Docker socket, deployment keys or GitHub PAT are mounted. Build
requirements (Clang, libcurl, Python/Pillow, Node 24) are baked into the image.

Unlike Ciaobella's JIT registration, these runners keep a repository-scoped
runner identity on the host and use `run.sh --once`. The container is removed
after each job; only the read-only runner identity is reused. GitHub registration
tokens are needed only during installation, so no administrative PAT is stored.
The runner identity is in `~gh-runner/.config/plus-runner/<slot>` (mode 700),
and is revoked by removing that runner in the repository's Actions settings.

## Maintenance

Install these files in `/home/gh-runner/plus-runner`, owned by `gh-runner`.
Copy the units to `~gh-runner/.config/systemd/user/`, reload the user manager,
build with `build-image.sh`, and enable `plus-runner@botty`,
`plus-runner@portal` and `plus-runner-image.timer`. Commands run as `gh-runner`
with `XDG_RUNTIME_DIR=/run/user/1001` and
`DOCKER_HOST=unix:///run/user/1001/docker.sock` on this server.

Register each runner once using a short-lived registration token from its
repository. Use names `dedie-botty-plus` / `dedie-portal-plus`, custom labels
`botty-plus-ci` / `portal-plus-ci`, `--unattended --disableupdate`, and save
`.runner`, `.credentials`, `.credentials_rsaparams` in its identity directory.
Pipe the token into `register.sh botty` or `register.sh portal` as `gh-runner`.
Never commit these files or registration tokens.

The image timer rebuilds weekly using the latest Actions runner and Node 24.
Each subsequent job picks up the new image. Inspect logs with
`journalctl --user -u plus-runner@botty -u plus-runner@portal`.

These repositories are public: PR code runs inside the container. Do not add
host filesystem mounts, Docker sockets or deployment credentials to these slots.
