# ASCII Desktop Overlay

A native Windows overlay that converts the primary desktop into live colored ASCII text. It uses Desktop Duplication for capture and GDI for rendering in a topmost, click-through window.

## Features

- Captures the primary monitor through DXGI Desktop Duplication.
- Excludes the overlay window from capture with `WDA_EXCLUDEFROMCAPTURE` to prevent recursive feedback.
- Renders a full-screen, borderless, topmost, mouse-transparent ASCII layer.
- Supports frame-rate, font, density, color boost, inversion, and primary-monitor settings.
- Adds a tray icon and global hotkeys.

## Controls

| Shortcut | Action |
|---|---|
| `Ctrl+Alt+A` | Show or hide the overlay |
| `Ctrl+Alt+Q` | Quit |

The tray menu can also toggle visibility, reload configuration, and exit.

## Build and run

Requirements: Windows 10 or newer, Visual Studio Build Tools with the C++ desktop workload, and a GPU/driver that supports Desktop Duplication.

```bat
Build ASCIIVision Native.bat
Launch ASCIIVision.bat
```

The executable is a local build artifact and is intentionally excluded from version control.

## Configuration

Edit `asciivision_native.ini` while the application is stopped, or reload it from the tray menu.

- `TargetFps` — requested capture/render rate.
- `FontName` and `FontHeight` — GDI font selection.
- `DensityPercent` — sampling density.
- `ColorBoostPercent` — source-color saturation/brightness scaling.
- `Invert` — invert the brightness-to-character ramp.
- `PrimaryMonitorOnly` — current implementation target.

## Privacy and limitations

Desktop pixels are processed in memory and are not written or transmitted by the application. The overlay can still expose on-screen information to people or recording software that can capture excluded windows. The implementation currently targets the primary monitor and Windows only. Protected or hardware-overlay surfaces may not appear in Desktop Duplication.

## License

MIT. Windows and DirectX components retain their platform licenses.
