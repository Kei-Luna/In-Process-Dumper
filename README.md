# In-Process-Dumper

Injected DLL that writes a dump of the process it is loaded into.

The repository intentionally contains only the dumper DLL. Load/inject it with
your preferred injector or version-shim loader.

## Build

Requirements:

- Windows
- Visual Studio Build Tools with MSVC
- CMake 3.20+

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

Output:

```text
build\Release\InProcessDumper.dll
build\Release\config.json
```

Use `-A Win32` instead of `-A x64` when the target process is 32-bit.

## Behavior

On `DLL_PROCESS_ATTACH`, the DLL starts a worker thread, opens a console window
when the process does not already have one, logs progress, and writes a minidump
of the current process with `MiniDumpWriteDump`. It also reconstructs the main
EXE image from process memory and writes it back to PE file layout. By default,
all output is written to the same directory as `InProcessDumper.dll` and the DLL
unloads itself after the dump is complete.

The DLL also exports `DumpCurrentProcess`, which can be called manually by a
loader that prefers explicit execution.

## Configuration

Settings are read from `config.json` next to `InProcessDumper.dll`.
Environment variables are still supported and override `config.json`.

Example:

```json
{
  "dump_dir": "",
  "dump_name": "",
  "exe_name": "",
  "log_name": "",
  "dump_flags": "0x00001026",
  "write_exe": true,
  "dump_modules": true,
  "aggressive_read": true,
  "dump_exec_regions": true,
  "unload": true
}
```

| Config key | Environment override | Description |
| --- | --- |
| `dump_dir` | `IPD_DUMP_DIR` | Directory for generated dump files. Defaults to the directory containing `InProcessDumper.dll`. |
| `dump_name` | `IPD_DUMP_NAME` | Full dump file path. If set, this overrides `dump_dir`. |
| `exe_name` | `IPD_EXE_NAME` | Full reconstructed EXE path. Defaults to `<dump name>.reconstructed.exe`. |
| `write_exe` | `IPD_WRITE_EXE` | Set to `false` to skip reconstructed EXE output. Defaults to enabled. |
| `dump_modules` | `IPD_DUMP_MODULES` | Set to `true` to reconstruct loaded DLL modules to `<dump name>.modules\*.dll`. Defaults to enabled. |
| `aggressive_read` | `IPD_AGGRESSIVE_READ` | Set to `true` to temporarily change committed unreadable page protections while reconstructing the EXE. |
| `dump_exec_regions` | `IPD_DUMP_EXEC_REGIONS` | Set to `true` to dump executable `MEM_PRIVATE` and `MEM_MAPPED` regions outside the main module to `<dump name>.exec_regions\*.bin`. |
| `log_name` | `IPD_LOG_NAME` | Full log file path. Defaults to `InProcessDumper_<PID>.log.txt` next to `InProcessDumper.dll`. |
| `dump_flags` | `IPD_DUMP_FLAGS` | Numeric `MINIDUMP_TYPE` flags, decimal or hex. Defaults to full memory, handles, thread info, and unloaded modules. |
| `unload` | `IPD_UNLOAD` | Set to `false` to keep the DLL loaded after dumping. Defaults to unload. |

A small status file is written next to the dump as `<dump>.status.txt` with the
dump path, reconstructed EXE path, process ID, and Win32 error codes (`0` means
success).

The same progress messages are also sent to the console and `OutputDebugString`.

The reconstructed EXE is rebuilt from the current main module in memory. Section
raw offsets and raw sizes are regenerated from virtual addresses and virtual
sizes, with a fallback that infers section size from the next section address.
This helps when a protector has cleared or minimized `PointerToRawData` and
`SizeOfRawData`.

Resources such as icons are copied if they are still present in the mapped
module. Data that is not mapped into the process image, such as an original file
overlay or a certificate table, cannot be recovered from memory-only dumping.

The log reports reconstructed EXE read quality:

```text
section=.text va=... size=... nonzero=... zero=... unreadable=...
Reconstructed EXE zero_bytes=... unreadable_bytes=... read_failure_bytes=... protect_recovered_bytes=...
```

If `zero_bytes` is high, retry with `IPD_AGGRESSIVE_READ=1`. Bytes recovered by
temporarily changing page protection are reported as `protect_recovered_bytes`.
Large remaining `unreadable_bytes` usually means the bytes are not committed or
not mapped in the process image.

If the main module sections are mostly zero but executable private/mapped regions
are produced with `IPD_DUMP_EXEC_REGIONS=1`, the unpacked or relocated code is
likely outside the original main module image.

Loaded DLLs are reconstructed from their in-memory `MEM_IMAGE` mappings when
`dump_modules` is enabled. This is useful when tools such as IDA ask for imported
DLLs while analyzing the reconstructed EXE.
