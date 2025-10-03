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