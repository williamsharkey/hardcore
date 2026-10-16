# Hardcore recordings

A `.hcrec` folder is a portable recording bundle. Open `index.html` in a
browser, or open the bundle from Hardcore's Clips TV to replay and
edit it. No server, package install, web font, or network connection is needed.

The bundle holds compressed camera video and the terminal renderer's draw
commands. It does not contain a screen recording. The browser draws the
original bitmap glyphs with nearest-pixel sampling, so changing playback size
does not substitute a browser font.

## Bundle files

- `manifest.json`: descriptive metadata.
- `replay.json`: the versioned recording data described below.
- `recording.js`: the same data assigned to `window.HARDCORE_RECORDING`; this
  lets `index.html` play from a local file without a fetch permission prompt.
- `camera.mp4`: camera/microphone media, when recorded.
- `index.html`, `core.js`, `player.js`, `style.css`: the standalone player.
- `edits.json`: optional nondestructive trim, crop, pointer, and caption edits.
- `edits.js`: a local-file-readable companion to saved edits.

The player also accepts an entire folder through its folder picker or drag and
drop. It parses a dropped `recording.js` as data; it never evaluates that file.
Clips opens replay/editing in an existing Hardcore browser tab and applies saved
edits through `applyEdits`. Exported MP4s also play inside Hardcore. The Files
action opens Hardcore's file viewer. Opening an older bundle refreshes its four
player assets; recording data, camera media, and saved edits are preserved.

## Replay version 1

All timestamps are milliseconds on the active recording clock. Paused time is
excluded. Frame and pointer arrays are ordered by timestamp; if frame times
are equal, the last frame wins.

```json
{
  "version": 1,
  "title": "A terminal session",
  "createdAt": "2026-10-16T12:00:00Z",
  "durationMs": 1000,
  "camera": {"file": "camera.mp4", "offsetMs": 0, "mirrored": true},
  "frames": [{
    "t": 0, "w": 1440, "h": 900, "bg": 2631720,
    "transparent": false,
    "quads": "",
    "atlas": [],
    "camera": {"rect": [1080, 600, 320, 240], "visible": true, "mirrored": true}
  }],
  "pointer": [{"t": 0, "x": 200, "y": 100, "visible": true, "buttons": 0, "type": "move"}],
  "captions": [{"start": 100, "end": 900, "text": "A caption"}]
}
```

`quads` is base64 of the native renderer's 20-byte instances, little-endian:
four signed 16-bit pixel edges `x0,y0,x1,y1`, four unsigned 16-bit atlas edges
`u0,v0,u1,v1`, then four RGBA bytes. `u0 == 65535` denotes a solid rectangle.
Draw order and source-alpha blending match the native renderer.

The atlas is 4096 × 4096 alpha pixels. Each frame's `atlas` array contains
patches `{x,y,w,h,data}`, with `data` as base64 of `w*h` alpha bytes in row
order. Apply patches before drawing the frame. The first frame supplies all
glyphs it references. Reused slots are overwritten by later patches. Seeking
backwards clears the atlas and reapplies patches through the target frame;
forward seeking applies even the patches in skipped frames.

Camera rectangles, cursor positions, and crop rectangles use source-frame
pixels. Camera media time is `(recording time - offsetMs) / 1000` seconds;
the camera stays hidden before its offset. The rectangle defines the camera
content area, and media fits inside it with centered black letterboxing. Captions render
after cropping, so they remain at the bottom of the output.

Pointer position holds at each event. Types are `move`, `down`, `up`, and
`scroll`; scroll events may carry `dx,dy`. A movement run begins after a gap
over 240ms. Cursor size is `14 + 9 * min(1, runAge/80) * max(0, 1-idleAge/240)`
pixels. A click draws an expanding ring for 450ms, with radius
`8 + 20*age/450` and opacity `1-age/450`. Native and browser export use this
same timing. Scroll events draw an amber arrow in the dominant scroll direction
beside the pointer, fading over 450ms.

The browser validates all draw commands when opening a recording, then keeps
decoded frame bytes in an 8MB least-recently-used cache. Seeking decodes evicted
frames as needed. The source JSON remains loaded for random access.

## Edits and host integration

Edits are `{version:1,startMs,endMs,crop,captions,pointer,captionItems}`. `crop`
is null for the whole frame, or `[x,y,width,height]`; the booleans control
captions and pointer cues. `captionItems` uses the replay caption format.
Import `.srt` or `.vtt` files with **Add captions** in a standalone browser.
Inside Hardcore, paste timed captions into the inline caption editor or drop a
caption file onto the replay tab. **Save edit settings** saves
edits in the native host or downloads `edits.json` in a standalone browser.

The embedded player sends messages to
`window.webkit.messageHandlers.hardcoreRecording` with actions `open`,
`library`, `files`, `saveEdits`, `export`, `zip`, and `output`. Save/export/ZIP
include the edit fields; output includes only a known job `id`. The host accepts
messages only from the opened bundle's canonical main document. Ordinary
browser tabs do not receive this bridge, even when viewing the same file.
The host can call `window.hardcoreRecordingPlayer.applyEdits(edits)`,
`seek(milliseconds)`, `pause()`, `hostStatus(job)`, or `hostTheme(palette)`.

MP4 and ZIP exports go to `~/Movies/Hardcore/Exports` with unique names. The job
manager keeps them running after the replay tab or Clips TV closes. Progress
and completed outputs appear in Clips and the replay tab. A replay tab's explicit
host marker and saved edits survive an app reload; a running export must finish
before the app reloads.

The native `recording-export` helper exports composed video from the original
commands. In a standalone browser, **Export video** shows the equivalent CLI
arguments. Trimming and cropping do not alter the original replay data.

Run `node tests/test_recording.js` to check draw-byte parsing, glyph restoration
across seeks, timestamp boundaries, pointer cues, captions, edits, and malformed
bundle rejection.
Run `node tests/test_recording_host.js` to check the embedded bridge UI without
opening a browser or capture device.
