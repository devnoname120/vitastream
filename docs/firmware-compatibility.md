# Firmware compatibility

Checked on 2026-10-11 using decrypted firmware files supplied locally. Firmware
binaries are not included in this repository. The machine-readable results are
in [firmware-compatibility.json](firmware-compatibility.json).

## Selection and validation

The plugin identifies the loaded `SceUdcd`, `SceAudio` and `SceKernelDmacMgr`
modules by NID. Each of the twelve released firmware families from 3.60 through
3.74 has an explicit identity entry. The internal 3.650.000 tool build has
three additional identities with the same verified code and layout.

Available external retail, testkit and devkit images have identical module
ELFs within each firmware family. Across releases:

- `SceAudio` executable code, the `0xBD24`-byte data layout and physical I2S
  register table are unchanged.
- `SceUdcd` has two executable variants, changing at 3.71. Both retain the
  configuration serializer at text offset `0x128C`, its argument/return ABI,
  and the interface/endpoint descriptor layout used by the plugin.
- `SceKernelDmacMgr` has two executable variants, changing at 3.63. Both retain
  the five-argument completion callback and DMAC4 interrupt affinity on CPU 3.
- The firmware's normal I2S output selector accepts indices 3 through 7.
  The register table maps 3, 4, 5 and 7; index 6 is unmapped. Audio admission
  verifies the current destination against the selected, enabled sink instead
  of requiring the handheld's I2S7 destination. It retains the normal-mode,
  secondary-output and idle-channel ownership guards.

Before installing the USB hook, the plugin verifies all three identities,
segment extents and relevant code contexts. The contexts contain no relocated
absolute addresses. Audio additionally verifies the physical I2S table and
rechecks its module before each capture session. Unknown identities, changed
contexts, undersized segments and failed lookups return without installing
hooks or arming DMA. A failed hook installation aborts startup, and later
startup failures release the installed hook.

Segment extents are checked using taiHEN's existing `module_get_offset`
interface. Its documented implementation permits the exclusive end of a
segment, so the checks request the complete required size rather than its last
byte. This also avoids introducing firmware-specific Modulemgr imports.
See [taiHEN's implementation](https://github.com/yifanlu/taiHEN/blob/master/module.c).

## External firmware evidence matrix

`Verified` means module identities, segment geometry, code contexts and every
linked kernel import were checked against that firmware's actual ELF files.
It does not mean a console was tested. Embedded modules are read directly from
decrypted `bootimage.elf` carriers when the archive has not unpacked them.

| Firmware family | Retail | Testkit | Devkit |
| --- | --- | --- | --- |
| 3.60 | Verified | Verified | Verified |
| 3.61 | Verified | Verified | Verified |
| 3.63 | Verified | Verified | Verified |
| 3.65 | Verified | Verified | Verified |
| 3.67 | Verified | Verified | Verified |
| 3.68 | Verified | Verified | Verified |
| 3.69 | Verified | Image unavailable | Verified |
| 3.70 | Verified | Verified | Verified |
| 3.71 | Verified | Image unavailable | Verified |
| 3.72 | Verified | Verified | External image unavailable; internal tool build verified |
| 3.73 | Verified | Verified | Verified |
| 3.74 | Verified | Image unavailable | Image unavailable |

There are 31 verified external combinations and five unavailable external
combinations. Additional internal/tool images are checked separately; they are
not substituted for missing external releases. The supplied
[firmware catalog](https://darthsternie.net/ps-vita-dev-firmwares/) lists an
internal 3.720.011 debugger build but no external images for those five cells.
Absence from these sources does not establish that a release never existed.

## Reproduce the checks

`make test` runs the existing transport, pacing and shutdown tests plus
sanitized compatibility and startup tests. The new tests cover all twelve
release identities, the internal alias, lookup failures, unknown identities,
changed contexts, short/misaligned segments, valid and invalid output sinks,
and startup cleanup after profile/hook/resource failures.

Build a plugin ELF with VitaSDK, then run:

```sh
python3 tests/firmware_matrix.py \
  --firmware-root /path/to/decrypted/external \
  --plugin-elf vitastream.elf \
  --output firmware-matrix.json
```

Repeat `--plugin-elf` to check multiple build variants, and use
`--additional-root /path/to/tool-image` for a separate extracted internal/tool
build. The check compiles the production selectors with the host C compiler,
matches them against original module bytes, tests rejection of altered input,
and verifies the plugin's actual linked library/function NIDs. taiHEN imports
are an external plugin dependency and are not Sony firmware exports.

The default and debug+diagnostic plugin builds use 67 and 77 Sony kernel
function imports respectively. Their import coverage is recorded per image in
the JSON report. Remaining build options are checked separately against the
same corpus. No Vita transfer, installation, reboot or runtime capture test was
performed during this compatibility work.
