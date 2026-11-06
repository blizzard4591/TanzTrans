# TanzTrans – Design

Tray application (Qt 6 Widgets, C++23) that watches a folder in a STRATO HiDrive share, finds phone videos
whose bitrate is higher than needed, transcodes them to HEVC Main10 MP4 with ffmpeg/libx265 and replaces the
originals on HiDrive (keeping a local backup), or – in read-only mode – saves the results locally.

User documentation: [README.md](README.md). Open points are marked **OPEN**.

## 1. Environment

| Item | Value |
|---|---|
| Platform | Windows 10 1803+ / 11, x64 (uses DPAPI, job objects, `NtSuspendProcess`, `tar.exe`) |
| Toolchain | MSVC (Visual Studio 2022/2026), CMake ≥ 3.25 + Ninja, Qt 6.11 `msvc2022_64` |
| Qt modules | Core, Gui, Widgets, Network, Sql (QSQLITE), LinguistTools, Test |
| Language | C++23 (`std::expected` as `tt::Result<T>`, ranges) |
| ffmpeg | ≥ 7.1 with libx265, found or downloaded at run time (§ 4) |

Source layout: `src/core` (static library, no widgets: settings, HiDrive client, probing, policy, ffmpeg,
database, job manager), `src/app` (GUI), `tests` (Qt Test, ffprobe samples with all tags removed),
`translations` (German).

## 2. Reference material

Two phone clips (not in git) drove the decisions:

| Sample | Video | Audio / extra |
|---|---|---|
| Samsung phone | 3840×2160, 60 fps (VFR), HEVC Main10, **HLG** BT.2020, ≈ 52 Mbit/s | AAC stereo 256k |
| iPhone | 1920×1080, 29.97 fps, H.264 High, SDR BT.709, ≈ 15 Mbit/s | AAC stereo, **APAC** spatial audio (ffmpeg cannot decode it), 6 `mebx` metadata tracks |

Benchmark, 300 frames of the 4K60 sample on a 12-core desktop CPU: x265 `medium` CRF 22 22 s (20 Mbit/s),
x265 `slow` CRF 22 47 s (25 Mbit/s), NVENC p7 CQ 24 8.5 s (34 Mbit/s). NVENC is 5× faster but needs ~35 %
more bitrate for the same quality, so libx265 on the CPU is used.

## 3. Encoding

- **Container** MP4, `-tag:v hvc1` (required by Apple), `-movflags +faststart+use_metadata_tags`.
- **Video** libx265, Main10 `yuv420p10le`, **capped CRF** in one pass: CRF + `vbv-maxrate` = cap,
  `vbv-bufsize` = 2 × cap. Default CRF 24, preset `slow`, below-normal process priority.
- **Frame rate** unchanged, timestamps passed through (`-fps_mode passthrough`; phones record VFR).
- **Resolution** unchanged; ffmpeg's autorotate bakes the rotation into the pixels.
- **HDR** HLG/BT.2020 stays HLG (primaries, transfer and matrix copied from the probe into the x265 VUI).
  Dolby Vision RPUs (iPhone HDR, profile 8.4) are dropped explicitly with `-dolbyvision 0` (ffmpeg ≥ 7.1
  defaults to *auto*); the HLG base layer remains. SDR stays BT.709.
- **Audio** the first AAC track is copied bit-exact; without AAC the first decodable track is encoded to
  AAC 192k. APAC and all data/metadata tracks are dropped (explicit `-map`).
- **Metadata** `-map_metadata 0` + `use_metadata_tags` keep creation time, GPS
  (`com.apple.quicktime.location.ISO6709`), make/model and the Apple creation date. The copied brand tags
  (`major_brand`, `minor_version`, `compatible_brands`) are cleared so they match the real `isom` header.
  The file date (local and on HiDrive) is set to the original's.
- **Marker** `comment=TanzTrans/1 x265 crf=<n> preset=<p> maxrate=<k>`: files carrying it are recognised
  as already transcoded even without the database.

```
ffmpeg -hide_banner -nostdin -y -i <src>
  -map 0:v:0 -map 0:<aac-index>
  -c:v libx265 -preset slow -crf 24 -pix_fmt yuv420p10le -profile:v main10 -dolbyvision 0
  -x265-params vbv-maxrate=<cap_k>:vbv-bufsize=<2*cap_k>:colorprim=<p>:transfer=<t>:colormatrix=<m>
  -tag:v hvc1 -fps_mode passthrough -c:a copy
  -map_metadata 0 -metadata major_brand= -metadata minor_version= -metadata compatible_brands=
  -movflags +faststart+use_metadata_tags -metadata comment="TanzTrans/1 ..."
  -progress pipe:1 -nostats <dst>.mp4
```

## 4. ffmpeg

**Finding it** (`Tools.cpp`, on a worker thread at start and after settings changes). Paths set in the
settings are used as they are; an empty ffprobe path means "next to ffmpeg". Empty paths mean automatic:
the first folder with a working `ffmpeg.exe` + `ffprobe.exe` pair wins, in this order:
1. next to `TanzTrans.exe`,
2. `%LOCALAPPDATA%\TanzTrans\ffmpeg` (the one-click download),
3. the Windows `PATH`, read fresh from the registry so a new install is found without a restart,
4. the winget link folders.

A pair is usable if both programs start, ffmpeg has the `libx265` encoder and knows `-dolbyvision` (7.1+).

**Without a usable ffmpeg** files are listed but not checked or encoded; the status bar says so and a
dialog offers the download (a tray message instead while the window is hidden). A tool that disappears
while running puts the file back without counting a failure.

**One-click download** (`FfmpegInstaller`): BtbN's GitHub builds, branch-pinned
`ffmpeg-n9.0-latest-win64-gpl-shared-9.0.zip` (falls back to the newest `n<x.y>` GPL shared release
build, never master). Steps: `checksums.sha256` → zip (SHA-256 checked) → unpacked with Windows'
`tar.exe` → `bin\` (without ffplay) plus `LICENSE.txt` and a `SOURCE.txt` into
`%LOCALAPPDATA%\TanzTrans\ffmpeg`. No administrator rights; the old copy is replaced only if it is not in
use. ffmpeg is downloaded on demand, not shipped, so TanzTrans does not redistribute GPL binaries.

## 5. When a file is transcoded

Resolution class = short side of the frame (portrait 2160×3840 counts as 2160p): ≥ 1800 → 2160p,
≥ 1300 → 1440p, ≥ 900 → 1080p, else < 1080p (any table order). Bitrates are for ≤ 30 fps and × 1.5
above 30.5 fps. The video bitrate comes from the stream's `bit_rate`, else the format bitrate minus audio,
else `size·8 / duration` minus audio.

| Class | Transcode above | Max bitrate (cap) | 50/60 fps |
|---|---|---|---|
| 2160p | 16 Mbit/s | 12 Mbit/s | 24 / 18 |
| 1440p | 10 Mbit/s | 8 Mbit/s | 15 / 12 |
| 1080p | 6 Mbit/s | 4.5 Mbit/s | 9 / 6.75 |
| < 1080p | 3 Mbit/s | 2.5 Mbit/s | 4.5 / 3.75 |

All values are settings. A transcode replaces the original only if it is ≥ 25 % smaller; otherwise the
file is marked *no gain* and the original stays.

## 6. HiDrive access

Verified against a real share (writes only in a test folder):

- **Link** `https://my.hidrive.com/share/<id>#$/<folder>`. The id addresses the whole share; the folder is
  only the URL fragment, so several links to one share have the same id.
- **Consequence** the share token can write anywhere in the share. TanzTrans takes its base folder from the
  fragment and checks every write (upload, rename, delete, date) against it; `..` is rejected. A link
  without a fragment makes the whole share the base folder.
- **Token** `POST /2.1/share/token` (`id`, optional `password`) → Bearer token, valid 4 h, renewed 5 min
  before expiry and after a 401. Writable, although the API docs call it read-only.
- **API** list `GET /dir` (names and paths are percent-encoded and decoded by the client); download
  `GET /file` with one byte range (resumes `.part` files); upload `PUT /file` (create/overwrite, first
  64 MB) then `PATCH /file?offset=`; `POST /file/rename` (keeps the date, 409 if the target exists);
  `DELETE /file`; `PATCH /meta?mtime=`; `GET /meta`.
- **Probing** runs remotely: ffprobe reads the HTTPS URL with the token in an `Authorization` header
  (≈ 1 s per file), so files below the threshold are never downloaded.
- **Secrets** link, share id, password and token are redacted from every log. The link and password are
  DPAPI-encrypted in the INI (or read from a link file). The token is visible on ffprobe's command line
  to processes of the same user for up to 4 h.

**Write mode** (setting, default **read-only**):
- **Read-only** results go to the local output folder (`Videos\TanzTrans`, mirroring the remote folders;
  names are made Windows-safe and can never leave that folder). HiDrive is never changed. Switching to
  replace mode offers to process these files again so their originals are replaced too.
- **Replace originals**, in this order, so the original is never deleted before the new file is in place:
  1. upload `<name>.tanztrans-part`;
  2. move the local original into the backup folder;
  3. pick the final name: an `.mp4` keeps its name, others become `<stem>.mp4`, or `<stem>_tt.mp4` if
     that is taken; if both are taken nothing is changed and the file is marked failed;
  4. same name: rename the original to `<name>.tanztrans-orig`, then the upload to `<name>` (the original
     is renamed back if that fails); new name: rename the upload;
  5. set the date, then delete the original (or the `.tanztrans-orig` copy) if it is unchanged.

  Each retry starts at step 1 and works out from HiDrive's state how far an earlier attempt got. A file
  whose `.tanztrans-orig` copy exists is not marked missing. Switching to read-only lets a running
  replacement finish.

## 7. Pipeline

```
Scan ──► Check (remote ffprobe) ──► Queue ──► Download ──► Encode ──► Verify ──► Replace / save locally
10 min    one at a time              newest     1 ahead      1 job,      local      one at a time
                                     first                   low prio    ffprobe
```

- **Scope** the base folder of the link, recursively; extensions `mp4 mov m4v mkv avi 3gp`; exclude
  patterns relative to the base folder.
- **Stability** a new file is checked once it was already there at the previous scan, or right away if
  its date is older than 30 min ("Treat files as complete after"). A known file counts as changed if its
  size or content hash changed (the date only when HiDrive gives no hash); it is then checked again.
- **Order** newest first (HiDrive date); manual move up/down/to top overrides it.
- **Concurrency** one encode at a time (x265 uses all cores); one download ahead; downloads need
  2 × file size + 2 GB free.
- **Verification** ffmpeg exit 0; HEVC, Main 10, `hvc1`; an audio stream; duration within 0.5 s + 2
  frames of the source; minimum saving reached.
- **Failures** a check is tried 3×; downloads, uploads and HiDrive calls 3× (retries after 1 and 2 min);
  an encode 2×. Then *failed* with a message and a tray notification; the queue continues. Stop and a
  missing ffmpeg never count as a failure.
- **Backups** originals are kept for 30 days (setting), then deleted – but not while the file is failed,
  unfinished or missing on HiDrive. If a path is replaced again later (a new file with the same name), the
  new original gets its own backup; the older one is kept until its own expiry.
- **Crash safety** ffmpeg runs in a job object that Windows kills with TanzTrans. At start, files left
  "running" go back one step; half-written outputs are deleted.

## 8. Controls

- **Pause** suspends the running ffmpeg process (`NtSuspendProcess`); Resume continues exactly there.
- **Pause after current job** pauses when the running encode ends, successfully or not.
- **Stop** aborts the encode and the transfers; the files stay queued (downloads resume from `.part`,
  encodes restart). TanzTrans stays stopped until Resume.
- **Schedule** always on by default; optional encode window (e.g. 22:00–07:00).

## 9. Database

SQLite (`tanztrans.sqlite`, WAL, `PRAGMA user_version` = 2; a newer version is refused, migrations go into
`Database::open`). Table `files`, key = remote path: HiDrive id, content hash, sizes, date, probe results,
decision values, status, priority, retries, retry time, local work files, output path/size, backup path and
expiry, job log path, message. Table `old_backups` (path, expiry): backups of earlier versions of a path.

| Status key | Meaning |
|---|---|
| `pending` | found, waiting to be checked |
| `probing` | being checked |
| `small` | bitrate below the threshold – nothing to do |
| `already-done` | carries the TanzTrans marker |
| `not-video` | no video stream |
| `queued` / `downloading` / `downloaded` | waiting for / during / after the download |
| `encoding` / `encoded` | transcoding / verified, waiting to be finished |
| `uploading` | finishing (replace on HiDrive or move to the output folder) |
| `done` | original replaced on HiDrive |
| `done-local` | result saved in the local output folder (read-only mode) |
| `output` | a file TanzTrans created on HiDrive |
| `no-gain` | transcode not small enough, original kept |
| `failed` / `skipped` / `missing` | gave up / excluded by the user / no longer on HiDrive |

## 10. Settings file

`%LOCALAPPDATA%\TanzTrans\TanzTrans.ini` (or `--config`):

| Group | Keys |
|---|---|
| `hidrive` | `link`, `password` (both `dpapi:`…), `linkFile`, `modifyRemote` |
| `monitor` | `pollMinutes`, `stableMinutes`, `extensions`, `excludes` |
| `storage` | `database`, `workDir`, `backupDir`, `outputDir`, `logDir`, `backupDays` |
| `tools` | `ffmpeg`, `ffprobe` (empty = automatic) |
| `encoding` | `crf`, `preset`, `highFpsFactor`, `bufsizeFactor`, `minGainPercent`, `rules/<n>/{minShortSide,thresholdMbps,capMbps}` |
| `schedule` | `lowPriority`, `windowEnabled`, `windowStart`, `windowEnd` |
| `app` | `language`, `autostart`, `minimizeToTray`, `notifications` |

## 11. User interface

Main window with tray icon (closing hides it), optional start with Windows (`HKCU\…\Run`).

- **Queue** columns File, Folder, Video (resolution, fps, HDR), Bitrate, Size, Status, Progress, Result;
  filter All / Queue and active / Problems / Finished. Context menu: Retry, Transcode anyway, Never
  transcode / Do not skip, Move to top / up / down, Open job log, Show local file in Explorer, Copy HiDrive
  path.
- **Log** application log with level filter, including the messages from start-up; one ffmpeg log per job.
- **Settings** HiDrive, Monitoring, Encoding, Schedule, Storage, Tools (ffmpeg), Application.
- **Toolbar / status bar** Scan now, Resume, Pause, Pause after current job, Stop; mode, state, progress
  with time left, next scan, space saved.
- **Languages** English and German (Qt Linguist), switchable without restart.

## 12. Verification status

- Unit tests (`tst_core`): link parsing and path guard, local path mirroring, ffprobe parsing of both
  samples, decisions and rule lookup, ffmpeg arguments, output verification, ffmpeg build selection.
- Read-only check of a real folder: thresholds and decisions as expected, no download, no change, no
  secrets in the log.
- Replace mode end to end in a test folder: `.mov` → `.mp4` and `.mp4` in place, HLG kept, APAC and data
  tracks dropped, metadata and dates kept, backups made, re-scan leaves the outputs alone.
- With the current replace order: same-name replacement through `.tanztrans-orig`, the `_tt` fallback
  when `<stem>.mp4` exists, no leftovers on HiDrive; killing TanzTrans mid-encode also ends ffmpeg (job
  object), and the restart deletes the partial output and encodes again without a new download.

**OPEN**
- Dolby Vision with a real iPhone HDR clip: check that the result plays as HDR (HLG) on an iPhone.
- Playback check of the test outputs on an iPhone, an Android phone and a 4K TV.
- The one-click download is unit-tested (checksums, build choice) but not yet run end to end.
