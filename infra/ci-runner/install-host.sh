#!/usr/bin/env bash
# Install reviewed infrastructure from an administrator account on dedie.
set -euo pipefail
cd "$(dirname "$0")"
sudo apt-get update
sudo apt-get install -y curl jq python3 sudo
# This account and its rootless Docker daemon already exist for Ciaobella.
[[ $(id -u gh-runner) == 1001 ]] || {
  echo "This dedicated-host configuration requires gh-runner UID 1001." >&2
  exit 1
}
# Required by Docker bridge inter-container isolation; persist across reboots.
sudo modprobe br_netfilter
printf 'br_netfilter\n' | sudo tee /etc/modules-load.d/plus-runner.conf >/dev/null
printf 'net.bridge.bridge-nf-call-iptables = 1\n' | sudo tee /etc/sysctl.d/99-plus-runner.conf >/dev/null
sudo sysctl -p /etc/sysctl.d/99-plus-runner.conf >/dev/null
[[ $(cat /proc/sys/net/bridge/bridge-nf-call-iptables) == 1 ]] || {
  echo "Enable Docker bridge filtering with: sudo sysctl -w net.bridge.bridge-nf-call-iptables=1" >&2
  exit 1
}
sudo loginctl enable-linger gh-runner
# This broker requires Ubuntu's existing GitHub login; no PAT is copied.
sudo -u ubuntu /snap/bin/gh auth status >/dev/null
for repository in Portablelle/Botty-Plus Portablelle/Portal-Plus; do
  sudo -u ubuntu /snap/bin/gh api "repos/$repository/actions/runners" >/dev/null
done
sudo install -o root -g root -m 755 api-broker.py /usr/local/sbin/plus-runner-api
sudo visudo -cf plus-runner.sudoers
sudo install -o root -g root -m 440 plus-runner.sudoers /etc/sudoers.d/plus-runner
sudo install -d -o gh-runner -g gh-runner -m 755 /home/gh-runner/plus-runner
for file in Dockerfile build-image.sh slot.sh stop-slot.sh plus-runner@.service plus-runner-image.service plus-runner-image.timer; do
  sudo install -o gh-runner -g gh-runner -m 644 "$file" "/home/gh-runner/plus-runner/$file"
done
sudo chmod 755 /home/gh-runner/plus-runner/{build-image,slot,stop-slot}.sh
sudo -u gh-runner env HOME=/home/gh-runner XDG_RUNTIME_DIR=/run/user/1001 \
  DOCKER_HOST=unix:///run/user/1001/docker.sock PATH=/home/gh-runner/bin:/usr/bin:/bin bash -c '
    set -euo pipefail
    docker info >/dev/null
    mkdir -p ~/.config/systemd/user
    cp ~/plus-runner/*.service ~/plus-runner/*.timer ~/.config/systemd/user/
    systemctl --user daemon-reload
    ~/plus-runner/build-image.sh
    systemctl --user enable plus-runner@botty plus-runner@portal plus-runner-image.timer
    # Installation/migration must replace any old persistent-identity process.
    systemctl --user restart plus-runner@botty plus-runner@portal
    systemctl --user start plus-runner-image.timer
  '
