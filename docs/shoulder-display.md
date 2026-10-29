# Optional Z7 shoulder display

This adds a read-only 128×96 shoulder panel to the existing browser UI. Pixels
come from original ex1610 functions executed in Unicorn using firmware supplied
locally by the user. No firmware, font, icon, captured payload or original
framebuffer is included. Contributions use GPL-2.0-or-later; the harness derives
from the existing WonderLantern shoulder tool with its license retained.

## Supported inputs

Only Nikon Z7 C3.80 is supported. Use extracted images, not the upgrade package.

| Image | Size | SHA-256 |
| --- | --- | --- |
| MAIN | 33052088 bytes | `e6df1764323fb1c287159dd3ce1c84ac93c72b96ba6eb09c070295c093fecbff` |
| ex1610 companion | 524288 bytes | `be90188211f7899cf974255a9c1527f589a5b5129e4cfc2d61efc545ea8fd42e` |

Install the optional dependency in the Python environment used by the launcher:

```sh
python3 -m pip install -r requirements.txt -r requirements-shoulder.txt
python3 tools/build.py
python3 tools/interactive.py --firmware /path/to/main-payload.bin \
  --companion-firmware /path/to/ex1610_037000.bin --profile menu
```

Open the launcher's printed localhost URL. The shoulder panel appears below
the main LCD after a valid request is observed. Existing shooting/photo options
can be used as before. This option does not enable the EX MCU test peer or
change the existing assistance profile. A disconnected peer can prevent MAIN
from producing snapshots; in that case the panel remains unavailable.

Without `--companion-firmware`, no Unicorn import is attempted and the observer
is disabled. Missing/unsupported firmware, missing Unicorn or a replay error
leaves the main camera service running. No host text, fonts or icon assets are
drawn into a replacement framebuffer. UI status text describes availability.

## Data path and boundaries

```text
MAIN display state service
  -> physical SPI0 TX (existing QEMU serial sink)
  -> optional passive Z7ShoulderObserver
  -> validated 256-byte wire frame in a local log
  -> shoulder_protocol.CaptureReader / parse_frame
  -> 112-byte logical channel 8, tag 4 snapshot
  -> ShoulderDisplay.receive: 0x080274BC
  -> explicit host call to publish: 0x08029854
  -> explicit host call to compositor: 0x080289F8
  -> framebuffer at 0x20018310 -> PNG -> browser panel
```

Physical SPI0 and logical channel 8 are distinct. The tag is the first byte of
the 112-byte payload. The 256-byte frame has an eight-byte header: channel,
routing byte, little-endian payload length and two opaque 16-bit words. Word
+4 is retained without claiming it is a sequence number. The checksum makes
the sum of all 256 bytes equal to 255 modulo 256; trailer bytes at 252/253 must
satisfy `byte252 >= byte253 > 0`. Other header fields retain unknown semantics.

The observer only samples accepted 16-bit SPI0 TX words. It assembles fixed
256-byte frames across FIFO completions; controller disable, configuration
changes and a virtual-time gap exceeding 100ms discard partial frames. There
is no byte-scanning recovery that could reinterpret a payload as a header.
Malformed frames and unrelated traffic are rejected. The Python parser
independently checks source, physical SPI, channel, tag, exact length, checksum
and trailer. Adjacent identical payloads skip redundant rendering. Only the
latest valid snapshot from each bounded log read is displayed.

Observation adds no guest memory/register writes, replies, ACKs or IRQs. Existing
serial behavior and optional diagnostic test peers remain controlled by their
original options. The log is a trusted local process channel, not a network
capture authentication mechanism. Its contents are private runtime artifacts.

## Native initialization and execution

The harness parses seven startup entries at `0x080038CC`, calls the original
copy/decompression/zero-fill routines and checks SRAM/source bounds. Entries
4–7 zero-fill their declared ranges. Decompression uses `0x0804809C` to
`0x2000D000`, length `0x1E58`; its SHA-256 must be
`e925d2bd3cc1e88f35317d54ce9f75b1fad136bcc1e244b7ee306a613f50817b`.
The 54-entry accessor table at `0x2000EA3C` must contain in-image Thumb pointers.
Fifteen literal references must still point to that table. No relocation or
firmware instruction patch is applied.

Only flash and SRAM are mapped. Guest flash is read/execute protected after
loading; invalid reads/writes/fetches raise Unicorn errors. Each native call
has a four-million instruction budget, a one-second execution timeout and must
return to the harness sentinel. A renderer is reused across updates and closed
on error, camera stop or restart. Restart creates a fresh initialized renderer.

Framebuffer geometry is 128×96, row-major 1bpp, most significant bit first,
16 bytes per row, exactly 1536 bytes. The host clears this buffer before calling
the original compositor, converts bits to two display colors, and encodes PNG.
It does not interpret exposure fields or draw Nikon glyphs itself.

`GET /api/shoulder` reports enabled/firmware_valid/available, the last successful
update time (Unix seconds), errors, renderer type, geometry and native evidence.
`GET /api/shoulder/frame` returns PNG or HTTP 503 when unavailable. The existing
`/api/status` includes the same shoulder status. HTTP handlers read published
copies under the camera lock; the existing worker alone owns QMP/RSP and calls
the shoulder service. Log reads are bounded to 256 KiB per refresh.

## Limits and verification

This is function-level replay, not complete MCU emulation. The MCU RTOS, SPI
wire receive driver, original event scheduler, display controller, electrical
interface and panel timing are not executed. The harness suppresses event
posting at `0x0802155A` by returning to LR inside its separate Unicorn instance;
it then calls publish and compose explicitly. MAIN guest execution is untouched
by this suppression. Do not describe this as natural MCU event execution or
hardware equivalence. Other cameras and firmware versions are unsupported.

Run `sh tests/run.sh` for firmware-free protocol/service/bit-order tests and
sanitized C observer contracts, plus the existing GPU/ISP tests. Optional private
integration checks use `Z7_SHOULDER_PRIVATE_CASES=/local/cases.json` with a JSON
object containing `firmware` and `cases` (each case has `payload` and `sha256`
for its local payload path and expected framebuffer digest). With Unicorn
installed, the same test runner checks native initialization, zero-fill,
receive/publish/compose, exact reference hashes and repeated updates. Keep
that manifest, payloads, images and results outside the public inventory.

Before publication, a human must review every changed file and its provenance,
then refresh `publication-manifest.json` and run `python3 tools/audit_publication.py`.
A stale manifest is a release gate, even when execution tests pass. See
[publication policy](publication.md) and [testing boundaries](testing.md).
