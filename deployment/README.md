# Deployment

Run repository commands from its root unless a step says otherwise. All domains,
addresses and SSH targets below are examples. Replace `botty.example.com` with
your domain, `203.0.113.10` with the server's address and `deploy@your-server`
with your own SSH account. Keep actual configuration in `deployment/local/`
(ignored by Git) or outside the repository.

## Host the portal

The portal is a static site. It needs no server-side Node.js process, database or
npm build. The following example targets Ubuntu with Nginx and systemd. Use a
trusted TLS certificate: installation verifies hashes through Web Crypto.

### 1. Verify and export locally

```sh
python3 scripts/portal-manifest.py --check
node --test tests/*.test.mjs
python3 scripts/portal-manifest.py --output dist/portal
```

The destination must not already exist. Choose a new directory for each release.
The export includes only current public packages, payloads, browser code, notices
and the manifest. Backups, source control, credentials and old app versions are
excluded. Keep application files byte-for-byte intact.

### 2. Upload a versioned release

On the server, install Nginx and create a release directory. Give your deployment
account ownership of the releases directory; the web server only needs read
access. The paths below assume the SSH account is named `deploy`.

```sh
sudo apt-get update
sudo apt-get install nginx certbot
sudo install -d -o deploy -g deploy /var/www/botty-ps5/releases
sudo install -d /var/www/botty-acme
```

From your workstation:

```sh
release_id=public-preview-1
ssh deploy@your-server "mkdir /var/www/botty-ps5/releases/$release_id"
rsync -a dist/portal/ "deploy@your-server:/var/www/botty-ps5/releases/$release_id/"
```

Verify the upload with the repository's standalone checker (it needs only Python):

```sh
ssh deploy@your-server "python3 - --check --root /var/www/botty-ps5/releases/$release_id" < scripts/portal-manifest.py
```

On the server, activate that release with an atomic symlink switch:

```sh
cd /var/www/botty-ps5
sudo ln -s releases/public-preview-1 current.next
sudo mv -Tf current.next current
```

For the first deployment, `current` must be absent; on later deployments it must
be a symlink. Inspect any existing path before replacing it. Retain older release
directories for rollback.

### 3. Configure the domain and TLS

Point your domain's DNS A record to the server. Add an AAAA record only if IPv6
works end to end. Allow inbound TCP 80 and 443 in your hosting firewall.

Copy `botty-portal.nginx` to `/etc/nginx/sites-available/botty-portal`, replace the
example domain, and **initially retain only the port-80 server block**. The TLS
block cannot load until the certificate exists. Enable the site:

```sh
sudo ln -s /etc/nginx/sites-available/botty-portal /etc/nginx/sites-enabled/botty-portal
sudo nginx -t
sudo systemctl reload nginx
sudo certbot certonly --webroot -w /var/www/botty-acme -d botty.example.com
```

Restore the TLS block from the example with your real domain and certificate
paths, then validate and reload:

```sh
sudo nginx -t
sudo systemctl reload nginx
sudo certbot renew --dry-run
curl -fsS https://botty.example.com/manifest.json
```

Keep the ACME challenge location for renewal and ensure successful renewals reload
Nginx (for example, a Certbot deploy hook running `systemctl reload nginx`).
The Nginx package's main configuration must include `mime.types` so ES modules
are served as JavaScript. The example disables directory listing and uses
`no-store` to avoid mixing versions. Do not put a generic SPA fallback, HTML
challenge, JS minifier or payload transformation in front of the portal.

Compare the served manifest against your verified local export:

```sh
curl -fsS https://botty.example.com/manifest.json -o /tmp/botty-served-manifest.json
cmp dist/portal/manifest.json /tmp/botty-served-manifest.json
```

Finally open the HTTPS portal on the PS5 and follow [console setup](../README.md#console-setup).
A desktop HTTP check does not prove that the PS5 trusts the certificate or that
home-screen discovery succeeds.

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

## Optional User's Guide DNS

Use this only if you need the PS5 User's Guide to open the hosted portal. Direct
browser access to the HTTPS portal does not need custom DNS.

1. Install `dnsmasq` on a host where port 53 is available. Resolve any conflict
   with the distribution's default dnsmasq instance or local DNS service before
   starting the dedicated unit.
2. Copy `botty-dns.conf` to `/etc/botty-dns.conf`. Replace `203.0.113.10` with a
   real address assigned to that host and replace `botty.example.com` with your
   portal domain. Behind NAT, use the appropriate local bind address and a
   reachable address for the Guide responses.
3. Restrict inbound **both UDP and TCP 53** to your trusted client/network
   addresses at the firewall **before** enabling the service. It must not become
   an unrestricted public resolver. Preserve your SSH firewall rule.
4. Install `botty-dns.service` into `/etc/systemd/system/`, then run:

   ```sh
   sudo dnsmasq --test --conf-file=/etc/botty-dns.conf
   sudo systemctl daemon-reload
   sudo systemctl enable --now botty-dns
   ```

5. Copy `botty-guide.nginx` into an enabled Nginx site and replace its redirect
   domain. The Guide hostname is not yours, so the example needs a local,
   self-signed certificate at `/etc/nginx/botty-guide-tls/guide.crt` and
   `guide.key`. Create those files before enabling the site:

   ```sh
   sudo install -d -m 0700 /etc/nginx/botty-guide-tls
   sudo openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
     -keyout /etc/nginx/botty-guide-tls/guide.key \
     -out /etc/nginx/botty-guide-tls/guide.crt \
     -subj /CN=manuals.playstation.net \
     -addext 'subjectAltName=DNS:manuals.playstation.net,DNS:manuals.playstation.com'
   sudo chmod 0600 /etc/nginx/botty-guide-tls/guide.key
   sudo nginx -t
   sudo systemctl reload nginx
   ```

6. Follow the [PS5 DNS and User's Guide walkthrough](../docs/CONSOLE-SETUP.md)
   to set the console's DNS fields and open the portal from Settings. Give users
   the reachable resolver IP and the expected HTTPS portal URL. A certificate warning or browser refusal may occur
   at the Guide hop; this route still needs real-console validation. The target
   Botty HTTPS domain must have its own trusted certificate.

The sample DNS configuration blocks the broad `playstation.com` and
`playstation.net` zones except the Guide and resolves other domains normally.
This can affect PSN features and is not a comprehensive update-blocking guarantee.
To undo it, restore the console's previous DNS settings. If the trusted client's
public IP changes, update the resolver's firewall allowlist.

## Updates and rollback

### Portal

For automatic publication from `main`, install the pull-based service below on
the portal server (Python 3.12+, Git, and outbound HTTPS to GitHub are required).
It checks `main` every minute, exports only manifest-verified public files,
rechecks the remote commit before activation, and atomically changes `current`.
A failed fetch or validation keeps the last working release. Concurrent runs
are locked. It retains the previous release and the three newest automatic
releases; manually created releases remain untouched. It does not update or
restart console processes.

```sh
sudo useradd --system --user-group --home-dir /var/lib/botty-portal --no-create-home botty-portal
sudo install -d -m 0755 /opt/botty-portal
sudo install -m 0644 deployment/sync-portal-main.py /opt/botty-portal/sync-portal-main.py
sudo chown botty-portal:botty-portal /var/www/botty-ps5 /var/www/botty-ps5/releases
sudo install -m 0644 deployment/botty-portal-sync.service deployment/botty-portal-sync.timer /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now botty-portal-sync.timer
sudo systemctl start botty-portal-sync.service
```

Set `BOTTY_PORTAL_REPOSITORY` in `/etc/botty-portal-sync.env` when using another
repository. The default public repository requires no GitHub or SSH secret.
Status and errors are available through `systemctl status botty-portal-sync.timer`
and `journalctl -u botty-portal-sync.service`. `/var/lib/botty-portal/last-deploy.json`
records the deployed commit and rollback target. Before a manual rollback, stop
the timer so it does not immediately restore the latest `main` release.
The `Portal checks` GitHub workflow runs the portal/installer tests, Python
regressions, and public manifest verification on pull requests and pushes to
`main`; it does not need deployment secrets or access to the server.

Export each update to a new directory, upload it to a new server release path,
verify it and switch `current` as above. Keep the previous target. Roll back by
switching the symlink to that retained release; do not overwrite a live directory
with a partial upload. Avoid switching releases while a console installer is
running, since a session fetches several files.

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
