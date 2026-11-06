# TanzTrans

Phones record video with far more data than needed: a 3-minute 4K clip easily takes 1–2 GB. TanzTrans watches
a folder in your STRATO HiDrive and converts such videos to a modern format (H.265 MP4). They look the same on
phones and TVs but usually need **60–80 % less space**. Recording date, location and the camera details are
kept.

## For users

### What you need

- A Windows 10 or 11 PC that is switched on for longer stretches. Converting is slow: a 3-minute 4K video
  takes roughly 20–30 minutes. TanzTrans works in the background, and you can keep using the PC.
- The **TanzTrans zip file**. Unpack the whole folder somewhere, for example to `Documents\TanzTrans`.
  Nothing needs to be installed.
- A HiDrive **share link** to the folder with the videos. To replace the originals, the link must allow
  uploads.
- **ffmpeg**, a free program that does the actual converting. When TanzTrans starts for the first time, it
  offers to download ffmpeg. Click **Download ffmpeg automatically**; it takes about a minute.

### First start

1. Double-click `TanzTrans.exe`. To use TanzTrans in German, open **Settings → Application → Language**.
2. In HiDrive, create a share link for the folder and copy it.
3. In TanzTrans, open the **Settings** tab and paste the link into **Share link**. Click
   **Test connection**, then **Apply**.
4. TanzTrans checks every video in the folder. Videos that are already small enough are left alone. The
   others are converted one after another.
5. At first TanzTrans works in **read-only** mode: HiDrive is not changed, and the converted videos are saved
   in your **Videos\TanzTrans** folder. Watch a few of them on your phone and TV.
6. If you are happy with them, tick **Replace originals on HiDrive** in the settings and click **Apply**.
   From now on TanzTrans uploads each converted video and removes the original from HiDrive. It keeps a copy
   of each original on your PC (see below). It also offers to do the same for the videos it has already
   converted.

### Everyday use

- TanzTrans lives in the **taskbar tray**, next to the clock. Double-click the icon to open the window.
  Closing the window keeps TanzTrans running; use **Quit** in the tray icon's menu to end it.
- The **Queue** tab shows every video and its state. Right-click a video to retry it, convert it anyway,
  never convert it, or move it up the list.
- **Pause** freezes the current conversion and **Resume** continues it. **Pause after current job** lets
  the current video finish first. **Stop** cancels the current video; it starts again later.
- To start TanzTrans with Windows, tick the option under **Settings → Application**.
- Problems show up under **Show: Problems** in the Queue tab and in red in the **Log** tab. Most go away
  with **Retry**.

### Getting an original back

TanzTrans keeps every replaced original on this PC for 30 days, in the same folder structure as on HiDrive.
To open the folder, press `Windows key + R`, type `%LOCALAPPDATA%\TanzTrans\backup` and press Enter. To
restore an original, upload it to HiDrive again.

### Removing TanzTrans

1. Untick **Start with Windows** and click **Apply**.
2. Use **Quit** in the tray menu.
3. Delete the TanzTrans folder.
4. Delete `%LOCALAPPDATA%\TanzTrans`, which also holds the backups of the originals and the downloaded
   ffmpeg. Check that you no longer need the backups first.

## For developers

How it works and why: [DESIGN.md](DESIGN.md).

**Requirements:**
- Windows, Visual Studio 2022/2026 (MSVC, C++23), CMake ≥ 3.25 and Ninja;
- Qt 6.11 `msvc2022_64` with Widgets, Network, Sql, LinguistTools and Test;
- for running it: ffmpeg ≥ 7.1 with libx265, found or downloaded at run time.

```powershell
./build.ps1                      # release: configure, build, unit tests, windeployqt into build\release
./build.ps1 -Preset debug        # debug build in build\debug
./build.ps1 -Package             # also dist\TanzTrans-<version>.zip for users (Qt + VC++ runtime included)
./build.ps1 -UpdateTranslations  # after changing UI strings; then translate in Qt Linguist
./build.ps1 -NoTests -NoDeploy   # build only
```

`QTDIR` defaults to `C:\Qt\6.11.2\msvc2022_64`. Run `ctest` or `tst_core.exe` directly only with `QTDIR\bin`
on the `PATH`.

Settings are stored in `%LOCALAPPDATA%\TanzTrans\TanzTrans.ini`; the share link and password are encrypted
for the current Windows user. The database, work folder, backups, logs and ffmpeg live in
`%LOCALAPPDATA%\TanzTrans\` as well.

| Command-line option | Purpose |
|---|---|
| `--config <ini>` | use another settings file (separate database, folders and link) |
| `--link-file <file>` | read the share link from this file for this run (not saved) |
| `--minimized` | start in the tray (used by "Start with Windows") |
| `--paused` | start paused: scan and check only, no downloads or encodes |
| `--exit-when-idle` | quit when nothing is left to do (with `--paused`: after all checks) |
| `--tab queue\|log\|settings` | open on this tab |
