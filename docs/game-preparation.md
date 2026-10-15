# Unified game preparation

The native settings sheet and web interface offer a download engine (PS5 or
Server), a destination disk, and **Prepare and add to Library** or **Download
only**. Preparation does not imply recompression. Its output goes directly onto
the selected disk; the final Library publication on that disk is a rename.

## Detection and plans

| Evidence | Preparation |
| --- | --- |
| First RAR volume (`.rar`, `.part01.rar`) | Extract once on the destination disk; retain original volumes. |
| PS5 folder (`sce_sys/param.json`, PPSA title and `eboot.bin`) | Publish the selected game folder. Local torrents retain a seeding copy. |
| exFAT, UFS2 `.ffpkg`, PFS `.ffpfs` / `.ffpfsc` signature | Publish the existing image without extracting or repacking it. ShadowMount must recognize it before readiness. |
| `.pkg` / `.fpkg` | Server-only Dump Forge inspection and streaming conversion to exFAT; transfer only the resulting image. |
| Multiple packages or archive sets | Require an explicit source choice; never choose a backport or DLC by filename. |
| Unsupported or ambiguous contents | Stop with an explanation; preserve originals. |

`FPKG` in a release name is a conservative local refusal signal. `DUPLEX` is a
publisher tag, not a folder-format signature. A bounded NFO read can explain
that a folder was announced, but cannot authorize an operation. In particular,
“Remove fakelib content if needed” never deletes files or changes conversion
options. Image-header recognition is not a claim that the game will launch.

Read-only Prowlarr sampling confirmed multipart RAR releases, standalone
unlockers, and torrents containing a base PKG, alternative backports and DLCs.
It also returned PS5 releases under an unrelated console category. No game
payload was downloaded and no torrent was added during that investigation.
Fixtures use synthetic names and files rather than tracker data.

## Storage and recovery

The automatic queue owns policy and destination. Torrent identity plus torrent
ID associates preparation with its source; display names never merge tasks.
Completed remote loose content transfers ownership of its private import root
to the preparation job by a journaled rename, then into Library by another
rename. This avoids a second PS5-sized copy. Local loose torrents keep their
original files for seeding and create one prepared copy; sharing writable game
files with the torrent would invalidate pieces. Archives retain their seed copy
and require space for one extracted output.

Cross-disk requests require one copy and retain the source until publication.
No target is overwritten. Files outside the selected torrent, links, and special
files are rejected. Interrupted ownership changes never automatically start a
second import. A completed private output can be recovered and published
without extraction again. Uncertain Library moves remain an explicit recovery
case instead of guessing which copy to remove.

Server conversion uses a private journal, process lock and source/output
identity checks. It serializes new conversions in one bridge process. The
converter streams to the final exFAT output; Botty does not create an expanded
folder or run an optional full byte comparison. It checks metadata, available
space and the resulting title. Source packages and successfully converted
server images are retained, allowing transfer retry without reconversion.
Interrupted/failed outputs require explicit retry; an uncommitted output is
preserved for inspection instead of being silently overwritten.

## ShadowMount and readiness

File preparation and Library registration are separate steps of the same job.
After publication, the job persists `libraryStatus=pending` and appears in
**Needs you**: **Close Botty+ and games, then return to the home screen**.
The manager waits until its PS5 native-process check confirms those apps are
closed before calling ShadowMount. This check alone cannot prove the foreground
screen; ShadowMount remains the authority and errors keep the job pending.
The job becomes ready only when ShadowMount returns the exact published path
with a PS5 title ID. Reopening Botty interrupts registration attempts, not the
completed files. No additional copy or extraction is scheduled while waiting.

Folder compression remains optional. Already packed images are not offered
folder compression. Image movement uses the existing ShadowMount transfer
path; image deletion requires its background-storage capability and a recognized
title. The fallback directory deleter never deletes images.

## Activity and UI

Activity defaults to **In progress**, with separate **Needs you**, **History**,
**Retained downloads** and **On Server** views. The original download is replaced
by its preparation while that job is tracked; live processing replaces its
matching job row. Failed preparations retain access to the source for retry.
Server candidates do not masquerade as active transfers.

Design references: screens 12–15 in
[Botty+ — UI Redesign](https://app.paper.design/file/01M4M010Q7XZ0F2CPTZJBD8PQ9/p-1-0).
The existing native Manrope/tokens and controller sheet layout are retained.

## Boundaries

Local FPKG additions are refused from torrent metadata and known release-name
hints. A magnet with no useful name can only be stopped once rTorrent exposes
its metadata; this is not a zero-byte guarantee. Archive headers are preflighted
before extraction and raw packages inside archives are refused on PS5. The
server converter currently accepts explicit package members, not opaque
PKG-inside-RAR collections. Such collections require server-side unpacking
before enrollment. Encrypted archives require a password and manual preparation.
No automatic backport removal, firmware choice, DLC installation, source cleanup,
or deployment is included. Console runtime acceptance is separate from host tests.
