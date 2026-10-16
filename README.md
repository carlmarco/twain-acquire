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

## License

MIT. See `LICENSE`.
