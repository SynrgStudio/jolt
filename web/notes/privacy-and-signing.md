---
title: Privacy and signing
status: stable
updated: 2026-11-11
---

<!-- From Jolt 0.5.0's docs page. -->

Jolt doesn't connect to the network. It will not transfer any information to other systems unless you ask it to. There is no telemetry and no account. The update check is off unless you turn it on (`check_updates = yes`, Settings, or the offer the installed copy makes once in the status bar): then, at most once a day, it asks GitHub's public API which is the latest release of Jolt. Nothing about you or your files is sent; GitHub sees the request and your IP address, as with any web page. When there is a new version, a click on the note in the status bar downloads it, checks its size, SHA-256 and version against what GitHub published, puts it in place and offers to restart. This site makes no third-party requests either.

Releases are built by [GitHub Actions](https://github.com/SynrgStudio/jolt/blob/master/.github/workflows/build.yml) from the repository's source, on GitHub's machines, and each one lists its SHA-256. They aren't signed yet: the plan is to apply to the [SignPath Foundation](https://signpath.org), which offers free code signing to open source projects, once Jolt is more widely used. Until then SmartScreen may warn on first run.

Jolt is free software under the [GNU GPL v3](https://github.com/SynrgStudio/jolt/blob/master/LICENSE): use it, change it, share it, as long as a modified version you distribute publishes its source under the same license.
