<p align="center">
  <img src="output/imagegen/botty-plus-github-og.jpg" alt="Botty+ banner" width="1280">
</p>

<h1 align="center">Botty+</h1>

<p align="center">Your downloads. Your library. On PS5.</p>

<p align="center"><strong>PS5 firmware 7.00–13.60</strong> · Requires a compatible jailbreak session. <a href="homebrew/botty-native/VALIDATION.md">Compatibility details</a></p>

<p align="center">
  <a href="https://github.com/Portablelle/Botty-Plus/releases/latest">Download Botty+</a> ·
  <a href="deployment/README.md">Installation guide</a> ·
  <a href="homebrew/botty-native/VALIDATION.md">Compatibility</a>
</p>

Botty+ lets you download, extract and manage your PS5 game library directly
from your console, using your controller. Downloads and file operations continue
in the background when you close the app.

Starting with 1.6.0, Botty+ uses one public version in the app and GitHub
releases. PS5 metadata is generated from it; component versions remain
available in System. See the [versioning policy](docs/VERSIONING.md).

## What you can do

- **Browse and download:** find content in Discover, by ranking or by searching every tracker, or add a magnet link or upload a `.torrent` file through the web interface. One sheet picks the source, disk and mode, then starts the download.
- **Follow every task:** Activity brings downloads, extraction, compression, moves and deletion together, with a Download → Extract → Library pipeline, progress and estimated time remaining.
- **Manage your Library:** view covers, compress supported games and see where each copy is stored. When both formats exist, their locations appear separately.
- **Use external storage:** choose the PS5 SSD or an external exFAT disk, check free space and transfer content between them.
- **Connect from another device:** open System to find the web address and login details for your phone or computer.

Discover merges all enabled Prowlarr torrent indexers. Choose a game, then
compare its tracker sources by size, seeders, grabs and date in the Get game
sheet before downloading. Discover requires optional Prowlarr setup. Manual magnet and torrent-file downloads work
without it. Extraction supports RAR archives; ZIP/7z extraction, PKG installation
and automatic game launching are not supported.

## Start here

You need a [compatible PS5](homebrew/botty-native/VALIDATION.md), enough storage
for downloads and extracted files, and a Botty+ portal hosted over trusted HTTPS.

### 1. Get access to Portal+

Botty+ is the console app and its background services. The jailbreak, DNS and
installer live in [Portal+](https://github.com/Portablelle/Portal-Plus).
Use a hosted Portal+ instance or follow its hosting guide. Portal+ automatically
publishes the verified Botty+ packages from this repository's `main` branch.
The native ZIP alone does not start the required background services.

### 2. Open the portal on your PS5

Open the portal's HTTPS URL using your working PS5 browser entry point. If your
host provides a User's Guide redirect, use the
[PS5 DNS and User's Guide walkthrough](https://github.com/Portablelle/Portal-Plus/blob/main/docs/CONSOLE-SETUP.md): it explains which
network settings to change, how to open the Guide and what to check if the
redirect fails. You need the DNS address supplied by your host for that route.

### 3. Install and launch Botty+

Enable **Botty+** in the launch options, then select **LAUNCH** once and keep the page open until it shows **READY**. The portal
prepares the native app and its background services. Press **PS**, return to the
home screen and open **Botty+**; allow some time for the icon to appear on the
first installation.

Repeat **LAUNCH** after a console reboot. For the screen layout and controller
shortcuts, see the [native app guide](homebrew/botty-native/README.md#screens-and-controls).

### 4. Add your first download

In **Activity**, press **Square** to add a magnet link. In the Get game sheet,
choose the PS5 SSD or an external exFAT disk, then **Extract and add to Library**
(Full auto) or **Keep the download only**, and press Cross. Botty+ remembers
both choices for the session. Follow the download and its extraction in
**Activity**.

To browse content in **Discover**, your host must first configure
[Prowlarr and the optional services](deployment/README.md#optional-search-and-explore).
To manage Botty+ from a phone or computer on the same network, open
**System** and use the displayed web address and login details.

### 5. Keep your installation up to date

With native 1.4.2 and the corresponding modern service stack, the app header
shows installation-wide update status. Open **System**, select **Update and
close**, and confirm **Install and close**. Close other native apps too. The app closes; the updater waits
for compression and file operations to finish, installs a compatible app/service
bundle and verifies the replacements before notifying you to reopen it. Downloads
pause only when their engine needs to restart, and updater-paused downloads are
resumed afterward. Real PS5 acceptance remains pending; see the [installation
update guide](homebrew/botty-native/README.md#installation-updates).

Use Portal+ for the initial installation and recovery. Activating this modern
stack requires a deliberately restarted idle console session after existing work
has finished, not a live-session service swap. It follows the
latest verified packages committed to Botty+ `main`. Close Botty+ before running
**LAUNCH**. If a service update is pending, let ongoing tasks finish before
restarting the console and launching again. See [updates and rollback](deployment/README.md#updates-and-rollback)
for recovery options.

## Everyday use

Choose **Full auto** to download, extract and add content to your Library, or
**Download only** to handle the next steps yourself. Pause, resume and verify
run at once from an item's details; select an item and press **Options** for
all its actions. Deleting always asks once more, with Cancel focused.

| Control | Action |
| --- | --- |
| L1 / R1 | Switch tabs: Discover, Activity, Library, System |
| D-pad / stick | Navigate; left/right changes a sheet row or filter |
| Cross / Circle | Select or confirm / go back or dismiss an error |
| Options | Quick actions, or compare sources in Discover |
| Square | Search all trackers in Discover, add a magnet in Activity |
| Triangle | Change the Discover ranking, filter Library or refresh Activity |

Close Botty+ from **System**; Circle never quits the app.

With the bundled ShadowMount, supported Library moves and deletions can run
while Botty+ stays open. Follow their progress in **Activity**; failures remain
visible. Compression activation/restoration and registration of new titles can
still require closing Botty+.

Compression keeps the original until you explicitly delete it. Full content
verification is optional; a skipped check is shown as **Not verified**. If
compression is interrupted, retrying on the same disk cleans its tracked
temporary files and starts from zero. Interrupted RAR extraction can reuse and verify partial output
on the same disk. Botty 1.5.1 requests experimental background-service retention during rest mode on
firmware 7.00–13.60 while its service is running. Long sessions can still be interrupted. Check the rest-mode status in the
web interface. FTP and a generated torrent download were tested in rest on 13.00;
upload, extraction, compression and other firmwares still need hardware validation.

## Preview

Discover leads with the focused game and its cover art. One **Get game** sheet
picks the source, disk and after-download step, with the button focused first.

![Discover](docs/screenshots/redesign-discover.png)

![Get game sheet](docs/screenshots/redesign-get-game-sheet.png)

Activity follows downloads, extractions and moves in one list. Destructive
actions confirm inline, with Cancel focused.

![Activity](docs/screenshots/redesign-activity.png)

![Quick actions with a delete confirmation](docs/screenshots/redesign-quick-actions-confirm.png)

*Rendered by the native UI on the host with sample PS5 titles, using covers from
an earlier console capture; not a console capture of this version.*

## Guides

- [Search/Explore setup and service updates](deployment/README.md)
- [PS5 User's Guide and DNS setup](https://github.com/Portablelle/Portal-Plus/blob/main/docs/CONSOLE-SETUP.md)
- [Native app guide](homebrew/botty-native/README.md)
- [Compression and recovery](homebrew/game-compressor/README.md)
- [External storage](homebrew/botty/README.md#external-storage-13)
- [Compatibility and known limitations](homebrew/botty-native/VALIDATION.md)
- [Building and contributing](docs/DEVELOPMENT.md)
- [Release notes and downloads](https://github.com/Portablelle/Botty-Plus/releases)

## Credits and licenses

Botty+ builds on Relapse and the open-source PS5 homebrew community.
See [credits and third-party licenses](THIRD_PARTY.md) for contributors,
dependencies and redistribution requirements.

Botty+ is an independent homebrew project, not affiliated with Sony Interactive
Entertainment.
