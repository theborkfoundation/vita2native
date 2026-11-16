# Bug reports

Review of commit `0beed00` (16 November 2026). All twenty reports have been
addressed; this file is kept as the record of what was wrong, where the fix
lives, and what is deliberately left as a limitation. The original descriptions
are in the history of this file.

## Fixed

| # | Report | Fix |
| --- | --- | --- |
| 1 | Different titles share and overwrite the same save data | `0a1ed2e` |
| 2 | Save APIs report success after failed writes | `ad0606c` |
| 3 | Long HTTP request paths read past a stack buffer | `03b21f8` |
| 4 | GXM transfers treat byte strides as pixel strides | `03b21f8` |
| 5 | Blocking message-pipe operations deadlock | `5f99b16` |
| 6 | Deleting a semaphore or event flag races its users | `cee80fd` |
| 7 | Cancelling semaphore and event-flag waits does not cancel them | `cee80fd` |
| 8 | GXM transfer notifications are never signaled | `03b21f8` |
| 9 | Sending to a closed socket can terminate the loader | `03b21f8` |
| 10 | `sceIoOpen` mishandles creation, exclusivity and access modes | `9373101` |
| 11 | Buffered `sceIoWrite` is not visible to other descriptors | `03b21f8` |
| 12 | Positioned I/O truncates 64-bit offsets on ARM32 | `9373101` |
| 13 | Simple-event timed waits expire immediately | `03b21f8` |
| 14 | Network errno values are Linux values, not Vita values | `fe94329` |
| 15 | Guest libc errno is never updated and is shared by all threads | `9373101` |
| 16 | Semaphore and event-flag deadlines restart on each wake | `03b21f8` |
| 17 | Audio reconfiguration silently drops most of a larger buffer | `ad0606c` |
| 18 | Two different VPKs can share a stale extraction cache | `ad0606c` |
| 19 | GXM transfer copy ignores layout and color-key modes | `fe94329` |
| 20 | HTTP accepts a truncated response body as a successful download | `ad0606c` |

## Regressions added

- `sync_smoke`: a bounded wait that is woken without being satisfied still
  returns at its deadline; a cancelled waiter reports `WAIT_CANCEL` and a later
  wait is unaffected; a waiter on a deleted semaphore reports `WAIT_DELETE`.
- `threadmgr_smoke`: a blocking receive on an empty pipe times out at its
  deadline instead of never returning.
- `net_smoke`: a nonblocking receive publishes `SCE_NET_EAGAIN`, not the host's
  11.
- `host_smoke`: `savedata0:` resolves per title, and a title name containing
  `..` cannot escape the savedata root.
- `apputil_smoke`: the saved file is located through the VFS rather than a
  hand-built path, and the test cleans its temporary root.

## Deliberate limitations

- **Transfers (#19).** Linear-to-linear copies support `COLORKEY_NONE`,
  `PASS` and `REJECT`. Tiled and swizzled types return
  `SCE_GXM_ERROR_UNSUPPORTED` instead of silently copying linear data; a title
  that used them now gets an error where it previously got wrong pixels.
- **Save-data migration (#1).** None, by design: there are no public releases,
  so the old shared `<root>/savedata` directory is simply no longer read.
- **Negative transfer strides (#4).** Handled, but no test exercises a
  bottom-up surface yet.
- **Network errno (#14).** The common errors are translated; anything not in the
  table passes through unchanged, so exotic failures can still report a Linux
  value.
- **Guest libc errno (#15).** The host value is copied as-is. Where Linux and
  the Vita's libc number an error differently, the guest sees the host number
  (the network API has its own translation).
- **Audio reconfiguration (#17).** Resizing restarts the stream, so frames
  queued before `sceAudioOutSetConfig` are dropped.
- **VPK identity (#18).** The stamp fingerprints the archive's first and last
  64 KiB; a deliberate collision is possible in principle, though not by
  accident.

## Cancellation, revisited

The deadlock and the discarded timeouts were fixed with #5 (`5f99b16`), but
`sceKernelCancelMsgPipe` still only woke its waiters, which then tested their
predicate again: a receive went back to sleep and a send could report success.
Both now compare a cancellation generation and return
`SCE_KERNEL_ERROR_WAIT_CANCEL` (`<see git log>`), the same rule semaphores and
event flags follow.
