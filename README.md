# Issue #8: VitaShell-only USB handoff candidate

[Download the 2026-11-09 test ZIP](vitastream-issue-8-vitashell-only-test-2026-11-09.zip)

This revision fixes an overly broad trigger in the previous handoff implementation.
The hooks now target SceMtpIfDriver's argument-taking Start/Stop wrappers and accept
only kind=1 requests from the VITASHELL process. Generic low-level SceUsbMtp shutdown
is no longer hooked. Missing optional handoff hooks no longer block capture startup.

The ZIP contains optimized normal and diagnostic plugins, corresponding GPL-3.0
source, the patch against 935acb1, build provenance, checksums and test instructions.
No automated or hardware tests were run for this revision. Static export metadata
contains the new hook functions and title-ID query in 31 available firmware images.

[Issue #8](https://github.com/devnoname120/vitastream/issues/8)

ZIP SHA-256: `de5e4334e66af79b20d999c20ad2c978d412cd1ef0e30e18edf9d6d3391051e3`

This is a distribution branch, not a release. Main is unchanged by this upload.
Older packages remain for comparison; their feedback does not validate this revision.
VitaStream is based on vita-udcd-uvc by Xerpi (GPL-3.0).
