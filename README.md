# twain-acquire

A TWAIN host for intra-oral X-ray acquisition. It opens a TWAIN data source,
negotiates capabilities, acquires images into a layout of slots (a mount), and
exports them as DICOM or TIFF to the imaging software that reviews them.

It acquires and exports only. It contains no sensor driver and does no
diagnostic image processing.

## Patient from the practice software

Practice software can start twain-acquire for the patient who is open. The
arguments are read with a template set in Setup > Options, for example

```
/PATID {id} /NAME {given} /SURNAME {family}
```

Placeholders: `{id}` `{family}` `{given}` `{birth}` `{sex}` `{accession}` and
`{skip}`. `{ssn}` is accepted and always discarded. twain-acquire's own flags
(`--patient-id`, `--family`, `--given`, `--birth`, `--sex`, `--accession`) are
always understood.

The patient can also come from a file of `key=value` lines, optionally under
`[section]` headers, in UTF-8 or the Windows code page. Name the file on the
command line with `{file}` in the template (for example `/FILE {file}`), or set
a file to watch that the practice software rewrites for the current patient.
Options > Handoff file says where each field is (`Key` or `Section/Key`). A
watched file found at startup is used only if it was written within the last
two minutes.

When twain-acquire is already running, a new launch passes its patient to the
open window and exits. A different patient never replaces one whose images are
not yet exported without asking, and is refused while the sensor is armed.

## Safety

- The armed state is shown to the operator for as long as a sensor is armed.
- A staged set is delivered only after the operator confirms it, once, before a
  short expiry. Anything doubtful delivers nothing.

## Building

Requires CMake 3.21+, a C99/C++17 compiler and Qt 6 (Widgets).

```
cmake -S . -B build -DCMAKE_PREFIX_PATH=<Qt prefix> -DTWAIN_INCLUDE_DIR=<dir with twain.h>
cmake --build build
ctest --test-dir build
```

The application runs TWAIN sources in a separate helper process, so its own
bitness does not have to match the source's. On Windows, build the helper a
second time for the other bitness and place it beside the application:

```
cmake -S . -B build32 -A Win32 -DTA_HELPER_ONLY=ON -DTWAIN_INCLUDE_DIR=<dir with twain.h>
cmake --build build32 --config Release
```

This produces `ta-helper32.exe`; the 64-bit build produces `ta-helper64.exe`.
The helpers can also be cross-compiled with MinGW-w64:

```
cmake -S . -B build-w32 -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-i686.cmake -DTA_HELPER_ONLY=ON -DTWAIN_INCLUDE_DIR=<dir with twain.h>
cmake --build build-w32
```

Use `cmake/mingw-x86_64.cmake` for the 64-bit helper.
Each helper loads the TWAIN Data Source Manager (`TWAINDSM.dll`) of its own
bitness. Without `twain.h`, the helper is built with simulated sources only;
run the application with `--simulate` to use them.

## Mounts

Built-in mounts: single image, 2 and 4 bitewings, an 18-image full mouth
series, 4 bitewings + 2 periapicals, and 4 periapicals + 2 bitewings. Setup >
Mount settings sets the rotation and mirroring applied to images taken into
each slot, and can mirror the whole mount. The changes are saved as a
`.layout` file in the data folder, which replaces the built-in mount of the
same id; Restore defaults removes it.

## Settings

Settings, export destinations, source profiles and layouts are plain text files
in the data folder (`--data-dir`, or the per-user application data folder), so a
configured office can be copied to another PC.

## Reliability

- An image that cannot be written (disk full, no permission) is held in
  memory and the window refuses to arm or close until it is retried, saved
  to another folder or discarded.
- The PC is kept from sleeping while the sensor is armed.
- If the application ends unexpectedly, the acquisition helper disarms and
  exits on its own, and is ended after a short grace period if a source is
  stuck.
- An export stuck on an unreachable network folder does not keep the program
  from closing; the job is retried at the next start.

## Patient data on this PC

Staged images and patient details stay in the data folder until exported.
Exported sessions, and the records of finished exports, are deleted after a
number of days set in Setup > Options (7 by default; "Never" keeps them).
Sessions that were not exported are never deleted automatically. The log file
holds no patient names and is capped at about 1 MB plus one older file.

## License

MIT. See `LICENSE`.
