# lockdetect

[![build](https://github.com/Mantis36558/lockdetect/actions/workflows/build.yml/badge.svg)](https://github.com/Mantis36558/lockdetect/actions/workflows/build.yml)

Find which process is holding (using / locking) a file or directory on Windows.

`lockdetect` uses the [Windows Restart Manager API](https://learn.microsoft.com/en-us/windows/win32/rstmgr/restart-manager-portal)
(`Rstrtmgr.dll`) to enumerate the processes currently using the given paths.

## Build

Requires [w64devkit](https://github.com/skeeto/w64devkit) (MinGW-w64 GCC) and CMake.

```bat
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
```

The executable is written to `build\lockdetect.exe`.

## Usage

```
lockdetect [options] <path> [path ...]
```

| Option            | Meaning                                                              |
|-------------------|----------------------------------------------------------------------|
| `-h`, `--help`    | Show help                                                            |
| `-v`, `--version` | Show version                                                         |
| `-b`, `--brief`   | Brief output: one `PID<TAB>name` line per unique locking process     |
| `-V`, `--verbose` | Verbose output: full executable, type, service, session, restartable |
| `-j`, `--json`    | JSON output                                                          |
| `-c`, `--csv`     | CSV output (header row included)                                     |
| `--`              | Treat all remaining arguments as paths                               |

With no format flag the output is grouped by path:

```
C:\dir\file.txt
  PID 1234  notepad.exe
```

### Examples

```bat
lockdetect C:\some\file.txt
lockdetect -b C:\some\file.txt C:\other
lockdetect -V C:\some\directory
lockdetect --json C:\some\file.txt
lockdetect --csv C:\some\file.txt > locks.csv
```

## Notes

- Restart Manager reports every process that currently has the file/directory
  mapped or open, not only holders of an exclusive lock. Some holders (e.g.
  kernel components) may not be reported.
- Requires Windows Vista or later.
- `-v` is reserved for `--version`; detailed output is `-V` / `--verbose`.
- Exit code is `0` on success, `1` if any path could not be queried, and `2`
  for a usage error.

## Download & verify

Prebuilt binaries are attached to each [GitHub Release](../../releases). The
`SHA512SUMS.txt` file next to the executable contains its SHA-512 checksum.

Verify with Git Bash / MSYS2:

```bash
sha512sum -c SHA512SUMS.txt
```

Or with PowerShell:

```powershell
(Get-FileHash .\lockdetect.exe -Algorithm SHA512).Hash.ToLower()
# compare the output with the hash line in SHA512SUMS.txt
```

## AI Disclosure

The source code in this repository was generated with AI assistance and
reviewed/edited by the author (Mantis36558).

## License

[BSD-2-Clause](LICENSE)
