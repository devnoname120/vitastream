# Issue #8 test build: automatic USB resume

This branch hosts the test package for [issue #8](https://github.com/devnoname120/vitastream/issues/8).
It is a distribution branch. The complete matching source is included inside the ZIP.

[Download vitastream-issue-8-autoresume-test-2026-11-07.zip](vitastream-issue-8-autoresume-test-2026-11-07.zip)

This candidate replaces the earlier package that required rebooting after storage.
It adds automatic USB video/audio device restoration after VitaShell ends storage,
including unplug -> end storage -> replug. Reconnection while storage is active
leaves the controller with storage. Host capture software may need to reopen its device.

The ZIP includes normal and diagnostic plugins, installation/check instructions,
complete corresponding source, SDK/build provenance and SHA-256 checksums.
Both plugins compiled successfully. No automated tests or hardware tests were run.

ZIP SHA-256: `9d35641fc1085bc8ff151b2d14e6a3d83d1d984f05a18813500c5463ef46cca5`

This is an untested candidate, not a release. See README-test.txt inside the ZIP.
VitaStream is based on vita-udcd-uvc by Xerpi (GPL-3.0).
