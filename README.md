# SFX-Parser

Windows tool for cutting sound effects out of long recordings. Open WAV / MP3 / FLAC files (each in its own tab),
mark start and end points on the waveform, and save every cut as a numbered 256 kbps MP3.

## Controls
- Left click: new start point. Right click: new end point. Drag a marker to move it (Ctrl picks the start when markers overlap).
- Delete while hovering a marker removes it.
- Mouse wheel zooms around the cursor; Shift+wheel or middle drag scrolls; Ctrl+wheel or the bar under the waveform scales vertically.
- Space or middle click (without dragging) plays the subsample under the mouse; Space elsewhere stops playback.
- The Save Subsamples button above the tabs saves the active tab only.
- Drop files on the window, or select several in File > Open.
- File > Save Cuts writes `OriginalName_01.mp3`, `OriginalName_02.mp3`, ...

## Build
Pure C++ / Win32, no dependencies (decoding and MP3 encoding use Windows Media Foundation). With w64devkit in `C:\workenv\`:

    build.bat

`tools/makeicon.cpp` regenerates `src/app.ico`.

## License
MIT, see [LICENSE](LICENSE).
