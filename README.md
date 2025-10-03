### Project Description: Snap Camera Capture Service

**Overview**:  
Snap is a lightweight Windows application (built as a native x64 executable using Visual Studio 2022) that captures frames from a connected webcam/video capture device using Microsoft Media Foundation (MF). It runs as either a background Windows service (named `snap`) or in console mode for testing. Captured frames are converted to WebP format for efficient storage and saved timestamped in a `temp` subdirectory. The project emphasizes self-contained deployment via static linking, avoiding runtime DLL dependencies.

**Key Features**:
- **Capture Mechanism**: Enumerates video devices, selects RGB32 or YUY2 formats, reads samples via `IMFSourceReader`, and converts YUY2 to BGRA/RGBA as needed.
- **Image Encoding**: Uses libwebp (statically linked via vcpkg) to encode RGBA frames to WebP at 80% quality, reducing file sizes compared to PNG.
- **Modes**:
  - **Service Mode**: Runs indefinitely, capturing every 10 seconds until stopped (e.g., via Service Manager). Logs to `temp\service.log`.
  - **Console Mode**: Captures every 10 seconds; exits on ESC key press.
- **Logging & Temp Management**: Creates/uses a `temp` folder in the exe directory for outputs; includes timestamped logging for debugging.
- **Build & Deployment**: 
  - Static linking with vcpkg (`libwebp:x64-windows-static`) and /MT runtime for standalone `snap.exe` (no DLLs like `libwebp.dll`).
  - Command-line build: `msbuild snap.vcxproj /p:Configuration=Release /p:Platform=x64 /p:RuntimeLibrary=MultiThreaded /p:VcpkgTriplet=x64-windows-static /p:VcpkgApplocalDeps=false`.
- **Dependencies**: MF libs (`mfplat.lib`, etc.), GDI+ (removed in WebP version), shlwapi, ole32; libwebp for encoding.

**Use Cases**: Ideal for surveillance snapshots, automated monitoring, or integration into larger systems needing periodic low-bandwidth image captures. Extensible for custom intervals, formats, or outputs.

**Source Code Structure**: Single-file C++ (wmain entry), with helpers for logging, conversion, and service control. Total ~500 LOC.

## Build Script

* Open `x86 Native Tools Command Prompt for VS 2022` and navigate to the project directory.

* Static Build Command:

```
msbuild snap.vcxproj ^
  /p:Configuration=Release ^
  /p:Platform=x64 ^
  /p:RuntimeLibrary=MultiThreaded ^
  /p:VcpkgTriplet=x64-windows-static ^
  /p:VcpkgApplocalDeps=false ^
  /m
```

## Create Service and Run
```
sc create snap binPath= "C:\dev\snap\x64\Release\snap.exe" start= auto DisplayName= "Snap Service"
sc description snap "This service runs the Snap application automatically on startup."
sc start snap
```

## Status Check, Stop and Delete Service
```
sc qc snap
sc query snap
sc stop snap
sc delete snap
```