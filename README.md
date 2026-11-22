# PopaVisual

Color correction and a crosshair overlay for games on Windows 10/11. Nothing is injected into the game:
saturation, contrast etc. are applied by Windows itself (Magnification API + gamma ramp), and the
crosshair, zoom and sharpening are drawn by a separate click-through window.

## Build

Visual Studio 2022, CMake 3.24+ and git. ImGui and nlohmann/json are fetched automatically.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The exe ends up in `build/Release` together with `Shaders.hlsl`. Settings are saved to `config.json` next to it.

## Hotkeys

- Insert - menu
- Ctrl+F1 - Ctrl+F4 - profiles
- End - reset filters of the current profile
- Zoom - hold the selected key (Mouse 5 by default)

## Notes

- Brightness, contrast, exposure, RGB, invert and gamma go through the hardware gamma LUT, and
  saturation through the GPU driver (NVIDIA Digital Vibrance / AMD saturation). Both cost 0 FPS and
  work in exclusive fullscreen. Without driver support (or with "DWM matrix" picked in Settings)
  saturation falls back to the DWM color effect, which does cost some FPS.
- The overlay window shrinks to the crosshair and hides when there is nothing to draw.
- Vibrance, sharpening and zoom use screen capture: +1 frame, borderless/windowed games only.
  Capture is off while none of them is enabled.
- In exclusive fullscreen only the gamma ramp applies.
- If Windows rejects the gamma ramp, create DWORD `GdiIcmGammaRange` = 256 under
  `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ICM` and reboot.
- Settings has the menu background (PNG/JPG, with blur) and the accent color.
- Some competitive games forbid third-party crosshairs or zoom.

## License

MIT
