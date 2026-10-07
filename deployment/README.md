# Botty+ services and updates

The console app and all required background services are delivered together by
[Portal+](https://github.com/Portablelle/Portal-Plus). Enable **Botty+** in its
launch options. Portal+ owns jailbreak, DNS, TLS hosting and installation;
this repository owns the download manager, native title, torrent engine,
compression worker and optional services described below.

Deployments must preserve torrent state, archives, saves and active jobs.
No credentials belong in source or public packages.

## Optional Search and Explore

Manual magnets, Transmission and extraction work without this integration.
Prowlarr itself is installed and managed separately; this repository supplies a
narrow reverse proxy and a console client, not a Prowlarr installation.

1. Run Prowlarr bound to `127.0.0.1:9696` on the server. Configure indexers and
   credentials in its private administration interface, accessed through SSH
   forwarding or a private network. Do not expose that interface through Botty.
2. Search and Explore query every enabled **torrent** indexer in Prowlarr using
   `indexerIds=-2` and the Console category tree (`1000`). Usenet results cannot
   be downloaded by the torrent service. Provider credentials stay in Prowlarr.
3. Botty combines results and ranks locally by seeders, Prowlarr `grabs`, or date.
   Explore requires a PS5 token in the title and displays up to 100 releases.
   Rankings cover the returned subset (up to 100 per indexer), not each tracker's
   entire catalogue. Missing grabs count as zero. Explore groups releases by game title before the PS5 token. Selecting a game
   opens a source chooser with tracker, release, size, seeders, leechers, grabs
   and date before storage, download mode and final confirmation. Each game retains
   up to 32 sources, ordered by seeders. Search uses duplicate info hashes, or exact
   normalized release titles and sizes when hashes are absent, produce one result with its sources;
   Botty prefers the source with more seeders and keeps the highest grabs count
   rather than adding counts. Separate editions/releases remain separate.
   Special ranking indexers and `exploreIndexers` mappings are no longer required;
   legacy mappings are ignored. Refresh Explore after changing providers to bypass
   its ten-minute cache.
4. Copy `botty-prowlarr.nginx` to `/etc/nginx/snippets/botty-prowlarr.conf`, enable
   its include in the portal's TLS block and run `nginx -t` before reloading.
   The proxy accepts positive numeric indexer download routes without exposing
   Prowlarr administration.
5. Copy `prowlarr.example.json` to a private working location, fill in the URL and
   Prowlarr API key, then provision it on the PS5 as `/data/botty/prowlarr.json`
   with mode **0600**. Use trusted console management tooling; the portal
   intentionally never provisions or receives this secret. Keep `/data/botty`
   private and remove temporary credential copies afterward.
6. Ensure `caFile` points to the installed service's CA bundle. Update it when
   switching service versions. The URL must use HTTPS, have no trailing slash,
   query, fragment or embedded credentials. The API key must be 32 hex characters.

The client reads the file for requests, so updating the config does not require
interrupting extraction. Search results use opaque IDs; upstream URLs and API
keys are not exposed to the native UI. Test Search and each Explore ranking
before selecting **Download and prepare**.

The automatic workflow enrolls items added through Search/Explore. It does not
silently enroll unrelated Transmission downloads. Passwords, multiple RAR sets,
missing volumes, CRC failures, unsupported outputs and destination collisions
require manual attention. Original download archives remain available for seeding.

## Optional cover resolver

The resolver listens on `127.0.0.1:9697`; Nginx exposes only its authenticated
artwork route. It uses the **same API key** as the console's Prowlarr configuration.

On the server:

```sh
sudo apt-get install python3-pil
sudo useradd --system --no-create-home --shell /usr/sbin/nologin botty-artwork
sudo install -d -o root -g botty-artwork -m 0750 /opt/botty-artwork
sudo install -m 0644 deployment/botty-artwork/server.py /opt/botty-artwork/server.py
sudo install -m 0644 deployment/botty-artwork/sources.json /opt/botty-artwork/sources.json
sudo install -m 0644 deployment/botty-artwork/aliases.json /opt/botty-artwork/aliases.json
sudo install -o root -g botty-artwork -m 0640 /dev/null /opt/botty-artwork/api-key
sudoedit /opt/botty-artwork/api-key
sudo install -m 0644 deployment/botty-artwork/botty-artwork.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now botty-artwork
sudo systemctl status botty-artwork
```

Skip user creation if the account already exists. Enter only the API key into the
key file; do not put it in shell arguments or source control. On updates, preserve
`api-key`, `aliases.json`, `sources.json` and `/var/cache/botty-artwork`; do not
repeat the command that creates an empty key file. Source mappings may have
local additions. See [resolver behavior](botty-artwork/README.md).

## Updates and rollback

### Console service

The portal stages updates into versioned manager directories and leaves a running
service alone. A pending update is explicitly shown after launch. Check the actual `/health` version; a portal update does not prove
the new daemon is active. Before a controlled restart, confirm there is no active
extraction or automatic job about to begin. Wait for work to finish; never kill an
active extractor merely to deploy. Preserve Transmission's process and state.

For routine updates, use the next deliberately restarted console session once
work is idle. Retain the previous service directory and a matching portal export.
If rolling back across a credential migration, stop Transmission cleanly first,
preserve its current state, and restore its settings and credentials as a matched
pair from your backups. Never erase torrent or resume directories.

### Native title

The portal upgrades recognized Botty installations automatically. Close Botty+ and
other native apps, then run **LAUNCH** from a fresh console session. It verifies the
new manifest and all thirteen staged files before moving the old title. Both the
fixed title ID/content ID and a recognized Botty title name are required; newer
installed versions are never downgraded. If the installed version is newer than
the portal package, **LAUNCH** keeps it untouched and continues starting the
services. Update the hosted portal to bring its package back in sync.

The previous tree is retained at
`/data/botty/native/backups/<transaction-id>/PPSA99071`. Previous registered metadata
is retained in the sibling `metadata/` directory. The updater refreshes only
Botty's known param/icon/background files under its registered title paths; it
never edits global application databases or other titles. Directory and data
permissions are checked. Normal library data, torrent archives and jobs are not
part of the update.

`/data/botty/native/update.json` records the transaction before either directory
move. A failed promotion restores the previous title when the live path is empty;
a later portal session resumes recovery after power loss. Uncertain or foreign
contents stop recovery for manual inspection rather than being deleted. Keep the
journal and backup until registration, home launch and controller navigation have
been verified. The new updater is host-tested; hardware acceptance remains open.

For manual recovery or rollback:

1. Close Botty+ and confirm it is no longer running. Inventory the title, journal
   and backup before moving any files.
2. Keep the current tree in a separate private directory. Restore the complete
   previous `PPSA99071` tree to `/data/homebrew/PPSA99071`, preserving permissions.
3. Restore matching metadata if needed. Files `0.bin`–`2.bin` correspond to
   `param.json`, `icon0.png`, `pic0.dds` under `/user/app/PPSA99071/sce_sys`;
   `3.bin`–`5.bin` to the same names under `/user/appmeta/PPSA99071`;
   `6.bin`–`8.bin` under `/system_data/priv/appmeta/PPSA99071`; `9.bin` is
   `/user/app/PPSA99071/icon0.png`. Only files that changed and previously existed
   are saved. Never restore these into another title's directory.
4. Use the matching older portal release if keeping the rolled-back version;
   otherwise the latest portal will offer the update again. Preserve the completed
   journal for diagnostics. If it is still pending, resolve its state before a
   further launch instead of discarding it blindly.
5. Refresh discovery through a fresh session and verify home launch and API health.
   For ftpsrv binary read-back, use `SELF` and confirm `SELF transfer mode disabled`
   before comparing raw FSELF hashes.

For a manual update outside the portal, stage the complete verified replacement
outside `/data/homebrew`, keep the existing title as a backup, then publish the
replacement while the app is closed. Never remove `/data/botty` to replace the UI:
it contains persistent downloads, jobs and credentials.
