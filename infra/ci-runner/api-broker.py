#!/usr/bin/python3 -I
"""Root-owned broker: only JIT creation/deletion for the two Plus repositories."""
import json
import os
import re
import subprocess
import sys
import time

REPOSITORIES = {"botty": "Portablelle/Botty-Plus", "portal": "Portablelle/Portal-Plus"}


def main():
    if os.geteuid() != 0:
        raise SystemExit("Install this broker as root and invoke it through sudo.")
    if len(sys.argv) not in (3, 4) or sys.argv[1] not in REPOSITORIES:
        raise SystemExit("Usage: plus-runner-api <botty|portal> <create|delete ID>")
    slot, operation = sys.argv[1:3]
    path = f"repos/{REPOSITORIES[slot]}/actions/runners"
    payload = None
    if operation == "create" and len(sys.argv) == 3:
        path += "/generate-jitconfig"
        method = "POST"
        payload = json.dumps({
            "name": f"dedie-{slot}-plus-{time.time_ns()}",
            "runner_group_id": 1,
            "labels": ["self-hosted", "linux", "x64", f"{slot}-plus-ci"],
            "work_folder": "_work",
        })
    elif operation == "delete" and len(sys.argv) == 4 and re.fullmatch(r"[1-9][0-9]*", sys.argv[3]):
        path += "/" + sys.argv[3]
        method = "DELETE"
    else:
        raise SystemExit("Only create and delete of a numeric runner ID are permitted.")
    # Reuse Ubuntu's existing gh login. Never hand its administrative token to gh-runner.
    command = ["/usr/sbin/runuser", "-u", "ubuntu", "--", "/snap/bin/gh", "api", path,
               "--method", method]
    if payload is not None:
        command += ["--input", "-"]
    reply = subprocess.run(command, input=payload, text=True, capture_output=True, timeout=30,
                           cwd="/home/ubuntu", env={"HOME": "/home/ubuntu", "PATH": "/usr/bin:/bin:/snap/bin", "LANG": "C.UTF-8"})
    if reply.returncode:
        # A JIT identity already disappears automatically after its job.
        if operation == "delete" and "(HTTP 404)" in reply.stderr:
            return
        sys.stderr.write(reply.stderr)
        raise SystemExit(reply.returncode)
    if operation == "create":
        data = json.loads(reply.stdout)
        if not isinstance(data.get("encoded_jit_config"), str) or not data["encoded_jit_config"]:
            raise SystemExit("GitHub returned no JIT configuration.")
        if not isinstance(data.get("runner", {}).get("id"), int):
            raise SystemExit("GitHub returned no runner ID.")
        sys.stdout.write(reply.stdout)


if __name__ == "__main__":
    main()
