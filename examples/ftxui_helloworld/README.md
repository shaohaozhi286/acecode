# FTXUI Hello World

Standalone CMake project for testing the repository's current FTXUI dependency.

Configure and build from the repository root:

```powershell
cmake -S examples/ftxui_helloworld -B build/ftxui_helloworld `
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DVCPKG_MANIFEST_DIR=C:/Users/shao/acecode `
  -DVCPKG_INSTALLED_DIR=C:/Users/shao/acecode/build/vcpkg_installed

cmake --build build/ftxui_helloworld --config Debug
```

Run:

```powershell
.\build\ftxui_helloworld\Debug\helloworld.exe
```

Press `q` or `Esc` to exit.

At startup, the demo prints `CONHOST: YES` or `CONHOST: NO` before entering the
TUI. After you press Enter, it plays a centered 3D `A` startup animation for
three rotations before creating the FTXUI screen. On Windows, `WT_SESSION` is
treated as Windows Terminal, a visible `ConsoleWindowClass` window is treated as
Windows Console Host (`conhost.exe`), and hidden pseudoconsole windows that
support virtual terminal output are treated as not conhost. When conhost is
detected, the demo also warns that some Windows 10 conhost setups render FTXUI
output poorly.
