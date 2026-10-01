# Botty+ 1.0 launch portal

This directory is the customized static Relapse portal. Press **LAUNCH** once on
a supported PS5 browser and keep the page open. The sequence verifies/installs
Botty+, loads Kstuff and ShadowMountPlus, starts FTP on 2121 and prepares
Transmission and the Botty service. After **READY**, press PS and open **Botty+**.
Home-screen registration is asynchronous. Restart after a failed session.

The bundled offset files cover 7.00–13.60. This does not establish full-stack
compatibility across that range. Hardware observations are limited to 13.00.

The full repository includes `README.md`, `deployment/README.md` and
`docs/DEVELOPMENT.md` with hosting, console setup, updates and build instructions:
[Botty+ repository](https://github.com/Portablelle/botty-ps5).

## Portal screenshot

This is the current launch portal shown on a supported PS5 browser. Select
**LAUNCH** to start the setup sequence.

![Botty+ PS5 launch portal](../docs/screenshots/portal-launch.png)

Host the complete verified export at the root of a trusted HTTPS origin. Package
verification requires Web Crypto. The browser fetches payloads and applications
from relative paths; there is no maintainer-hosted domain dependency. Do not
rewrite package contents, cache incompatible releases together or serve local
backup directories. `manifest.json` inventories the exported files.

The installer preserves existing matching files and running services. A recognized older native title is updated with the app closed after staging and
verification, with its previous directory retained in private storage. Foreign
titles and downgrades are refused. Interrupted swaps recover through a journal.
A newer service is staged while an old daemon continues its active work; it starts
on the next console restart. FTP is loaded again after reboot;
successful payload delivery is not proof that a payload initialized correctly.

## Upstream credits

The browser stage uses JavaScriptCore information leaks and a structured-clone
object pool mismatch. The kernel stage combines an address leak with an
`aio_multi_wait` use-after-free race.

- Sonic_Iso: kernel exploit.
- Jordy: WebKit exploit and kernel bug.
- ntfargo and ufm42: exploit development.
- Dr. Yenyen: testing.
- Additional contributors: TheFlow, SlidyBat, Flatz, cow, nhk, bollarz,
  Sleirsgoevy, EchoStretch and EarthOnion.

Upstream revision is recorded in `manifest.json` and in the repository's
`Relapse-Exploit` submodule. Preserve the upstream `LICENSE` and attribution.

The bundled ShadowMountPlus `1.7beta2-botty.1` includes Botty's guarded TitleDir
recovery patch. Its [notice](payloads/shadowmountplus-NOTICE.md),
[GPL license](payloads/shadowmountplus-LICENSE.txt) and
[complete corresponding source](payloads/shadowmountplus-source.tar.gz)
are included in this portal.
