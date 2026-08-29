# IPC bridge protocol — removed

This document described the named-pipe/JSON protocol between the extension DLL and the standalone
`voice-client/` process. That two-process architecture is gone: audio capture, networking, and DSP
now live inside the extension DLL itself (`addon/extensions/task_force_radio_pipe/src/Voice/`),
talking directly to `voice-server` over the protocol in
[`protocol-network.md`](protocol-network.md). There is no longer an IPC bridge to document.

Kept as a stub rather than deleted outright since other docs and old commit messages still refer
to this file by path.
