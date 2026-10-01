// Same-console delivery after Relapse has created the userland ROP runtime.
// Payloads are fetched from this site's pinned release files, never from user input.
const SYS = { WRITE: 4, CLOSE: 6, SOCKET: 97, CONNECT: 98 };
const FILES = ["kstuff.elf", "shadowmountplus.elf", "ftpsrv-ps5.elf", "ProsperoMgr.elf"];
const pause = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

export async function sendPayload(runtime, name, fetchFile = fetch, delay = pause) {
  if (!FILES.includes(name)) throw new Error("Unknown payload.");
  if (!runtime || !runtime.p || !runtime.chain) throw new Error("Run Jailbreak first.");
  const response = await fetchFile("./payloads/" + name, { cache: "no-store" });
  if (!response.ok) throw new Error("Payload download failed: HTTP " + response.status);
  const bytes = new Uint8Array(await response.arrayBuffer());
  if (bytes.length < 4096 || bytes.length > 16 * 1024 * 1024 ||
      bytes[0] !== 0x7f || bytes[1] !== 0x45 || bytes[2] !== 0x4c || bytes[3] !== 0x46)
    throw new Error("Invalid ELF payload download.");

  const { p, chain } = runtime;
  const buffer = p.malloc(bytes.length, 1);
  buffer.backing.set(bytes);
  const address = p.malloc(16, 1);
  // FreeBSD sockaddr_in: length=16, AF_INET=2, port=9021, 127.0.0.1.
  address.backing.set([16, 2, 0x23, 0x3d, 127, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0]);

  for (let attempt = 0; attempt < 20; attempt++) {
    const fd = (await chain.syscall(SYS.SOCKET, 2, 1, 0)).low | 0;
    if (fd < 0) throw new Error("Could not create the payload socket.");
    let connected = false;
    try {
      connected = ((await chain.syscall(SYS.CONNECT, fd, address, 16)).low | 0) === 0;
      if (connected) {
        for (let offset = 0; offset < bytes.length;) {
          const length = Math.min(65536, bytes.length - offset);
          const written = (await chain.syscall(SYS.WRITE, fd, buffer.add32(offset), length)).low | 0;
          if (written <= 0 || written > length)
            throw new Error("Payload transfer interrupted. Restart the PS5 before trying again.");
          offset += written;
        }
        return bytes.length;
      }
    } finally {
      await chain.syscall(SYS.CLOSE, fd);
    }
    await delay(250);
  }
  throw new Error("ELF loader is not accepting connections on port 9021.");
}
