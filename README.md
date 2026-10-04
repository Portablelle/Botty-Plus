<p align="center">
  <img src="output/imagegen/botty-plus-github-og.jpg" alt="Botty+ banner" width="1280">
</p>

<h1 align="center">Botty+</h1>

<p align="center">Your downloads. Your library. On PS5.</p>

<p align="center"><strong>PS5 firmware 7.00–13.60</strong> · Bundled Relapse jailbreak range. <a href="homebrew/botty-native/VALIDATION.md">Compatibility details</a></p>

<p align="center">
  <a href="https://github.com/Portablelle/botty-ps5/releases/latest">Download 1.4.3</a> ·
  <a href="deployment/README.md">Installation guide</a> ·
  <a href="homebrew/botty-native/VALIDATION.md">Compatibility</a>
</p>

Botty+ lets you download, extract and manage your PS5 game library directly
from your console, using your controller. Downloads and file operations continue
in the background when you close the app.

## What you can do

- **Browse and download:** find content through Search and Explore, or add a magnet link or upload a `.torrent` file through the web interface. Pause, resume and check your downloads.
- **Follow every task:** Processing brings extraction, compression, moves and deletion together, with progress and estimated time remaining.
- **Manage your Library:** view covers, compress supported games and see where each copy is stored. When both formats exist, their locations appear separately.
- **Use external storage:** choose the PS5 SSD or an external exFAT disk, check free space and transfer content between them.
- **Connect from another device:** open Connections to find the web address and login details for your phone or computer.

Search and Explore merge all enabled Prowlarr torrent indexers. In Explore,
choose a game, then compare its tracker sources by size, seeders, leechers, grabs
and date before downloading. Search and Explore require optional Prowlarr setup. Manual magnet and torrent-file downloads work
without it. Extraction supports RAR archives; ZIP/7z extraction, PKG installation
and automatic game launching are not supported.

## Start here

You need a [compatible PS5](homebrew/botty-native/VALIDATION.md), enough storage
for downloads and extracted files, and a Botty+ portal hosted over trusted HTTPS.

### 1. Get access to a portal

If someone already hosts Botty+ for you, ask them for the portal URL and skip to
step 2. To host it yourself, download **botty-portal-1.4.3.tar.gz** from the
[latest release](https://github.com/Portablelle/botty-ps5/releases/latest) and
follow the [hosting guide](deployment/README.md#host-the-portal). It covers
uploading the complete site, configuring HTTPS and checking the installation.

Use the complete portal bundle: the native app ZIP alone does not install the
required background services.

### 2. Open the portal on your PS5

Open the portal's HTTPS URL using your working PS5 browser entry point. If your
host provides a User's Guide redirect, use the
[PS5 DNS and User's Guide walkthrough](docs/CONSOLE-SETUP.md): it explains which
network settings to change, how to open the Guide and what to check if the
redirect fails. You need the DNS address supplied by your host for that route.

### 3. Install and launch Botty+

Select **LAUNCH** once and keep the page open until it shows **READY**. The portal
prepares the native app and its background services. Press **PS**, return to the
home screen and open **Botty+**; allow some time for the icon to appear on the
first installation.

Repeat **LAUNCH** after a console reboot. For the screen layout and controller
shortcuts, see the [native app guide](homebrew/botty-native/README.md#screens-and-controls).

### 4. Add your first download

In **Downloads**, press **Square** to add a magnet link. Choose the PS5 SSD or an
external exFAT disk, then select **Full auto** to download, extract and add the
result to your Library, or **Download only** to handle those steps yourself.
Follow the download in **Downloads** and the extraction in **Processing**.

To browse content through **Search** and **Explore**, your host must first
configure [Prowlarr and the optional services](deployment/README.md#optional-search-and-explore).
To manage Botty+ from a phone or computer on the same network, open
**Connections** and use the displayed web address and login details.

### 5. Keep your installation up to date

Your host updates the portal using the latest complete bundle. Close Botty+
before running **LAUNCH** from that updated portal. If a service update is
pending, let ongoing tasks finish before restarting the console and launching
again. See [updates and rollback](deployment/README.md#updates-and-rollback)
for the detailed procedure and recovery options.

## Everyday use

Choose **Full auto** to download, extract and add content to your Library, or
**Download only** to handle the next steps yourself. Select an item and press
**Options** for its available actions.

| Control | Action |
| --- | --- |
| L1 / R1 | Switch tabs |
| D-pad / stick | Navigate |
| Cross / Circle | Open or confirm / go back |
| Options | Item actions |
| Square | Search, add a magnet or another screen-specific action |
| Triangle | Refresh or change the Explore ranking |

With the bundled ShadowMount, supported Library moves and deletions can run
while Botty+ stays open. Follow their progress in **Processing**; failures remain
visible. Compression activation/restoration and registration of new titles can
still require closing Botty+.

Compression keeps the original until you explicitly delete it. Full content
verification is optional; a skipped check is shown as **Not verified**. If
compression is interrupted, retrying on the same disk cleans its tracked
temporary files and starts from zero. Interrupted RAR extraction can reuse and verify partial output
on the same disk. Keep the console awake while tasks are running.

## Preview

Library shows **Compressed**, **Uncompressed** or **Both formats**, plus the
location of each copy. Missing disks are marked **Offline**.

![Library copy and storage labels](docs/screenshots/library-copies-1.3.2.png)

*Preview rendered by the native UI with sample data.*

![Explore tracker chooser](docs/screenshots/explore-trackers-1.3.6.png)

*Tracker chooser rendered by the native UI with sample data.*

![Background file operations](docs/screenshots/background-processing-1.4.0.png)

*Processing rendered by the native UI with sample data.*

## Guides

- [Hosting, Search/Explore setup and updates](deployment/README.md)
- [PS5 User's Guide and DNS setup](docs/CONSOLE-SETUP.md)
- [Native app guide](homebrew/botty-native/README.md)
- [Compression and recovery](homebrew/game-compressor/README.md)
- [External storage](homebrew/botty/README.md#external-storage-13)
- [Compatibility and known limitations](homebrew/botty-native/VALIDATION.md)
- [Building and contributing](docs/DEVELOPMENT.md)
- [Release notes and downloads](https://github.com/Portablelle/botty-ps5/releases)

## Credits and licenses

Botty+ builds on Relapse and the open-source PS5 homebrew community.
See [credits and third-party licenses](THIRD_PARTY.md) for contributors,
dependencies and redistribution requirements.

Botty+ is an independent homebrew project, not affiliated with Sony Interactive
Entertainment.
