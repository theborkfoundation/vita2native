# Bug reports

Review of commit `0beed00` (16 November 2026). Priorities are based on impact and likelihood, not on whether a particular title is known to hit the bug. The scenarios below use the existing exported functions; they do not require changes to guest code.

## Validation and scope

- On the Orange Pi Zero 3, the existing `host_smoke`, `sync_smoke`, `threadmgr_smoke`, `apputil_smoke`, `http_smoke`, and `net_smoke` tests passed via `tools/v2n test`.
- Small, separate ARM32 harnesses linked the project's existing objects and ran through `tools/v2n test`. They reproduced the device observations below. The audio harness substituted a recording host sink so that it could count submitted frames without depending on the sound device. These harnesses live under the ignored `build/review/` directory and intentionally return failure to make `tools/v2n test` print their observations.
- The HTTP long-path test used AddressSanitizer on macOS with the same `SceHttp.c` implementation; only the ELF export-registration macros were replaced for the host build. The VPK cache and truncated HTTP response were also exercised locally. Other findings explicitly marked **Code review** were not reproduced on a device.
- This is a targeted review, not an exhaustive audit. The project intentionally executes guest code without a sandbox; the reports below concern incorrect behavior within the supported API paths.

## 1. Different titles share and overwrite the same save data — P1

**Locations:** [main.c](source/main.c#L989), [host_fs.c](source/host/host_fs.c#L253), [SceAppUtil.c](source/modules/SceAppUtil.c#L86).

Every launch in a working directory calls `host_fs_init` with the same `vita2native` root, and `savedata0:` always resolves to `<root>/savedata`, independent of the title. Save-slot metadata is also named solely by slot number. The supplied kiosk switches titles while retaining its working directory, so two games using `savedata0:/data.bin`, slot 0, or `sce_sys/safemem.dat` address the same files. The second game can read, overwrite, or delete the first game's saves; even slot creation can fail because another title already created that slot.

**Reproduced on ARM:** initializing the filesystem with the same root and two different app directories resolved `savedata0:/data.bin` to exactly the same host path both times.

**Suggested fix:** namespace save storage by stable title identity, including VPK launches, and provide an explicit migration strategy for the existing shared directory. Add a two-title regression that writes the same filename and slot number in each title and checks both persist independently.

## 2. Save APIs report success after failed writes — P1

**Location:** [SceAppUtil.c](source/modules/SceAppUtil.c#L94), [data save](source/modules/SceAppUtil.c#L274).

`write_slot_param` ignores both `fwrite` and `fclose`. `sceAppUtilSaveDataDataSave` ignores a failed `fseek`, a short or failed `fwrite`, `fclose` errors, directory-creation failures, and the return from `write_slot_param`. It can therefore tell a game that saving completed when nothing or only part of the save reached storage. A buffered write can appear to succeed until `fclose`, which is especially relevant when storage fills up.

**Reproduced on ARM:** a save file mapped to `/dev/full` returned 0, as did a save with offset `-1` whose seek failed. Both are error conditions. The test used temporary paths, not a real title's saves.

**Suggested fix:** validate offsets and propagate errors from directory creation, seek, the complete write, and close/flush; return a suitable Vita error and do not report the slot update as successful after a failed data write. Test with a failing sink and a short/failed write.

## 3. Long HTTP request paths read past a stack buffer — P1

**Location:** [SceHttp.c](source/modules/SceHttp.c#L571).

The request path can be nearly 2048 bytes, but the request line is formatted into `header_line[512]`. When it is truncated, `snprintf` returns the length that *would* have been written; that larger length is passed to `buffer_append`, which copies past the end of the stack array. A game requesting a long URL, including one received via a redirect, can crash or send adjacent stack contents to the server.

**Reproduced with AddressSanitizer:** a local HTTP request with a 700-byte path produced a 716-byte read from the 512-byte `header_line` in `buffer_append`.

**Suggested fix:** size the request line dynamically or reject truncation before copying; apply the same rule to every `snprintf` result used as a copy length. Exercise long paths and redirect locations under a memory sanitizer.

## 4. GXM transfers treat byte strides as pixel strides — P1

**Locations:** [fill](source/modules/SceGxm.c#L9694), [copy](source/modules/SceGxm.c#L9749), [downscale](source/modules/SceGxm.c#L9787); compare the vendored [Vita3K transfer implementation](external/Vita3K/vita3k/renderer/src/transfer.cpp#L45), which interprets linear stride in bytes.

All three transfer functions multiply the supplied row stride by bytes per pixel. A multi-row 32-bit surface with an 8-byte stride therefore advances 32 bytes per row instead of 8. This produces incorrect images and can read or write beyond the guest's actual surface allocation.

**Reproduced on ARM:** filling a 2×2 `RAW32` surface with stride 8 wrote the second row at byte offset 32; byte offset 8 remained untouched. The harness deliberately allocated extra space to observe the offset safely.

**Suggested fix:** use signed byte strides correctly for linear transfers, audit the source and destination calculations in all three operations, and test multi-row surfaces with padding and nonzero coordinates. Handle any supported negative-stride semantics explicitly.

## 5. Blocking message-pipe operations deadlock while holding their own mutex — P1

**Locations:** [SceKernelThreadMgr.c](source/modules/SceKernelThreadMgr.c#L1167), [receive](source/modules/SceKernelThreadMgr.c#L1198), [host_thread.c](source/host/host_thread.c#L261).

The send and receive paths lock `info->lock` and then call `condvarWait`, which attempts to lock that same nonrecursive host mutex again before calling `pthread_cond_wait`. Consequently, the thread blocks on itself without releasing the mutex; the other side cannot acquire it to make progress. The existing `condvarWaitLocked` helper has the contract these callers need. The exported blocking pipe functions also explicitly discard their timeout parameters, and cancellation merely broadcasts without recording cancellation.

**Reproduced on ARM:** a receive on an empty pipe with a 100 ms timeout was still blocked after 300 ms; a subsequent send also blocked. The isolated harness was stopped by its watchdog. This failure occurs even if a sender is available later.

**Suggested fix:** wait using the locked variant, implement one deadline for the whole operation, and give cancel/delete explicit wake-up results. Test empty receive, full send, timeout, cancellation, and handoff between two threads.

## 6. Deleting a semaphore or event flag races its users — P1

**Locations:** [semaphore lookup and wait](source/modules/SceKernelSync.c#L78), [delete semaphore](source/modules/SceKernelSync.c#L218), [delete event flag](source/modules/SceKernelSync.c#L550).

**Code review:** lookup returns a raw pointer after releasing the table lock. Delete removes the entry, broadcasts, then destroys the condition variable and mutex and frees the object without waiting for users or marking it deleted. A blocked waiter wakes and may re-enter the wait loop because its predicate is unchanged; another caller may already hold the returned pointer. Depending on scheduling and pthread behavior, deletion can hang, access destroyed synchronization objects, or use freed memory. The same pattern exists for event flags.

**Suggested fix:** keep objects alive while any operation holds a reference; mark deletion under the object lock, wake waiters with `SCE_KERNEL_ERROR_WAIT_DELETE`, and destroy only after all references and waiters have left. Test deletion during a blocked wait and concurrent lookup/delete.

## 7. Cancelling semaphore and event-flag waits does not cancel them — P1

**Locations:** [semaphore cancel](source/modules/SceKernelSync.c#L326), [event-flag cancel](source/modules/SceKernelSync.c#L687), [wait loop](source/modules/SceKernelSync.c#L128).

Cancellation updates the count or pattern and broadcasts, but waiters only recheck the ordinary success predicate. If it remains false, they wait again; if the new count/pattern satisfies it, they return success. Neither case reports `SCE_KERNEL_ERROR_WAIT_CANCEL`. This can hang shutdown and recovery paths that cancel workers before joining them.

**Reproduced on ARM:** canceling an empty semaphore reported one waiter, but it was still blocked 100 ms later. After a normal signal it returned 0 rather than a cancellation error. Event flags have the equivalent control flow and were inspected, not separately run.

**Suggested fix:** track cancellation generations or per-wait state under the lock, wake the affected waiters with the cancellation result, and keep later waiters independent of an earlier cancellation.

## 8. GXM transfer notifications are never signaled — P1

**Locations:** [fill](source/modules/SceGxm.c#L9674), [copy](source/modules/SceGxm.c#L9720), [downscale](source/modules/SceGxm.c#L9766), [notification wait](source/modules/SceGxm.c#L3365).

Each transfer ignores its `notification` argument and returns without writing the requested value. `sceGxmNotificationWait` loops until the value appears, so a guest that waits on the transfer completion can hang forever even though the CPU-side transfer already completed. The vendored [Vita3K implementation](external/Vita3K/vita3k/modules/SceGxm/SceGxm.cpp#L5425) signals this notification after the transfer.

**Reproduced on ARM:** a successful `sceGxmTransferFill` with a notification requesting value 77 left its initialized notification word at 0. The harness inspected the word rather than entering the unbounded wait.

**Suggested fix:** publish the notification after a successful transfer with appropriate ordering, and honor the related sync-object contract. Add a transfer-and-wait regression for all three entry points.

## 9. Sending to a closed socket can terminate the loader — P1

**Locations:** [SceNet.c](source/modules/SceNet.c#L205), [sendmsg](source/modules/SceNet.c#L794), [SceHttp.c](source/modules/SceHttp.c#L447), [signal setup](source/main.c#L779).

The host sends without suppressing `SIGPIPE`, and the installed guest signal handlers do not include it. On Linux, writing to a closed stream can therefore terminate the entire process instead of returning a socket error to the guest. This affects `sceNetSend` and related send paths, and plain HTTP uses the same kind of host `send`.

**Reproduced on ARM:** in an isolated child process, `sceNetSend` on a socket whose peer had closed terminated the child with signal 13 (`SIGPIPE`).

**Suggested fix:** suppress SIGPIPE consistently for all host socket writes, including TLS behavior, and translate the resulting error into the appropriate guest-facing result. Test both SceNet and HTTP against a peer that closes early.

## 10. `sceIoOpen` mishandles creation, exclusivity, and access modes — P1

**Location:** [SceLibKernel.c](source/modules/SceLibKernel.c#L123).

The `fopen` mode mapping ignores `SCE_O_EXCL`; `SCE_O_APPEND` maps to `a`/`a+` and `SCE_O_TRUNC` maps to `w`/`w+`, both of which create missing files even when `SCE_O_CREAT` is absent. An ordinary existing `SCE_O_WRONLY` file is opened as `r+`, requiring read permission and permitting reads through a write-only descriptor. The requested creation mode is ignored as well.

**Reproduced on ARM:** `WRONLY|CREAT|EXCL` opened an existing file, `WRONLY|APPEND` created a missing file without `CREAT`, and reading from an ordinary `WRONLY` descriptor returned one byte.

**Suggested fix:** translate the Vita flags to `open(2)` flags and mode, preserving exclusive creation and access permissions atomically, then use descriptor-based I/O. Test each flag combination on existing and missing files.

## 11. Buffered `sceIoWrite` is not visible to other descriptors — P1

**Location:** [SceLibKernel.c](source/modules/SceLibKernel.c#L236).

`sceIoWrite` calls `fwrite` on a buffered `FILE *` and returns its result without flushing. A second descriptor can see the old contents or EOF after the first write has already reported success. This differs from the file-descriptor API exposed to the guest and can break save verification, cross-thread readers, and database-style access. Buffered stream errors are also not consistently converted to negative guest errors.

**Reproduced on ARM:** after writing one byte to a newly created file, a separately opened reader returned 0 bytes until the writer was closed.

**Suggested fix:** use unbuffered descriptor operations for `sceIo*`, preserve error results, and test read-after-write across two open descriptors without a close or explicit sync.

## 12. Positioned I/O truncates 64-bit offsets on ARM32 — P1

**Location:** [SceLibKernel.c](source/modules/SceLibKernel.c#L675).

`sceIoPread` and `sceIoPwrite` accept a 64-bit `SceOff` but cast it to 32-bit `long` for `fseek`, and save the current position using `ftell`. Offsets at and above 2 GiB can become negative or otherwise wrap. The save-and-seek sequence also is not atomic with respect to other operations on the same descriptor, so concurrent positioned and sequential I/O can interfere with each other's file position.

**Reproduced on ARM:** a one-byte `sceIoPwrite` at 3 GiB returned `0x80010009` instead of writing to a sparse file at that offset.

**Suggested fix:** use 64-bit `pread`/`pwrite` on a real descriptor, or otherwise serialize operations and retain full-width offsets. Test around 2 GiB and concurrent operations sharing one descriptor.

## 13. Simple-event timed waits expire immediately — P1

**Locations:** [event creation](source/modules/SceKernelEvent.c#L79), [event wait](source/modules/SceKernelEvent.c#L210).

The event's condition variable uses the default `CLOCK_REALTIME`, while its absolute deadline is calculated from `CLOCK_MONOTONIC`. On a normal Linux system the latter timestamp is far in the past relative to realtime, so any unsatisfied timed wait immediately returns `SCE_KERNEL_ERROR_WAIT_TIMEOUT`.

**Reproduced on ARM:** waiting for an unset simple-event bit with a 250,000 µs timeout returned the timeout error in 0 ms.

**Suggested fix:** initialize this condition variable with a monotonic clock, or calculate its deadline using the clock the condition variable actually uses. Test an unsignaled timeout and a signal arriving before its deadline.

## 14. Network errno values are Linux values, not Vita values — P1

**Locations:** [SceNet.c](source/modules/SceNet.c#L198), [errno location](source/modules/SceNet.c#L310), [Vita constants](external/vita-headers/include/psp2common/net.h#L173).

The socket functions return host failures directly and `sceNetErrnoLoc` returns the host `errno` pointer, but Vita's network error numbering differs from Linux. A common example is nonblocking `EAGAIN`: Linux returns 11 while `SCE_NET_EAGAIN` is 35. Similar differences affect `EINPROGRESS`, connection errors, and other network conditions. Guest retry and nonblocking-connect logic can take the wrong branch.

**Reproduced on ARM:** receiving from an empty nonblocking socket returned -1 with guest-visible errno 11, while the Vita header defines `SCE_NET_EAGAIN` as 35.

**Suggested fix:** maintain per-thread guest network errno and translate host errors at each failed operation, including validation failures and relevant socket options. Cover nonblocking receive and connect failures.

## 15. Guest libc errno is never updated and is shared by all threads — P1

**Locations:** [SceLibc.c](source/modules/SceLibc.c#L62), [fopen wrapper](source/modules/SceLibc.c#L626).

`_sceLibcErrnoLoc` returns one global integer, but the libc wrappers call host functions that update the host's thread-local `errno`; no code copies that value to the guest integer. A failed file operation can thus leave guest errno at zero or at a stale value. Multiple guest threads also overwrite the same guest errno location.

**Reproduced on ARM:** opening a nonexistent save file returned `NULL`; host errno was 2 (`ENOENT`) while the guest errno location remained 0.

**Suggested fix:** expose correctly translated, per-thread guest errno and update it on the relevant failing operations. Test a failed `fopen` and independent failures in two threads.

## 16. Semaphore and event-flag deadlines restart on each wake — P2

**Locations:** [semaphore wait](source/modules/SceKernelSync.c#L128), [event-flag wait](source/modules/SceKernelSync.c#L464).

Both loops recompute `now + *timeout` after each wake, including signals that do not satisfy the wait and spurious wakes. A bounded wait can therefore exceed its requested duration indefinitely if unrelated notifications keep arriving.

**Reproduced on ARM:** a semaphore wait for count 2 with a 200 ms timeout received one insufficient signal at 100 ms and returned timeout after approximately 301 ms. The event-flag loop has the same deadline placement and was inspected, not separately run.

**Suggested fix:** calculate one absolute deadline before entering the loop, and preserve the Vita API's timeout output semantics. Test repeated insufficient signals.

## 17. Audio reconfiguration silently drops most of a larger buffer — P2

**Locations:** [port allocation](source/modules/SceAudio.c#L232), [resampling](source/modules/SceAudio.c#L149), [set config](source/modules/SceAudio.c#L419).

The output scratch capacity is calculated only at port creation. `sceAudioOutSetConfig` can later increase `len` or lower the input sample rate without resizing it. `resample_port` stops when the old scratch buffer fills, then returns success and advances past the full input buffer, dropping the unprocessed frames. This also affects audio-thread pacing.

**Reproduced on ARM with a recording sink:** open a stereo port at 256 frames/48 kHz, reconfigure it to 1024 frames/48 kHz, and submit one buffer. The call returned success but queued only 260 frames.

**Suggested fix:** resize scratch and queue capacity for the new configuration, or process the entire buffer in bounded chunks. Test both an increased length and a lower input frequency.

## 18. Two different VPKs can share a stale extraction cache — P1

**Locations:** [stamp](source/vpk.c#L78), [cache check](source/vpk.c#L173), [shared destination](source/main.c#L916).

All VPKs in a working directory extract into the same `vita2native/app0`, but the cache stamp contains only archive size and mtime in whole seconds. If a different archive has the same size and timestamp, extraction is skipped. The loader can then run the new VPK's executable with the previous VPK's assets and libraries.

**Reproduced locally:** two distinct 118-byte VPKs with the same mtime contained `data.txt` values `AAAA` and `BBBB`. Extracting the first and then the second both returned success, but the extracted file still contained `AAAA`.

**Suggested fix:** key the extracted directory or stamp by archive identity and content, and publish successful extraction atomically. Test switching between same-size, same-mtime archives.

## 19. GXM transfer copy ignores layout and color-key modes — P2

**Location:** [SceGxm.c](source/modules/SceGxm.c#L9720).

**Code review:** `sceGxmTransferCopy` discards `srcType`, `destType`, `colorKeyMode`, `colorKeyValue`, and `colorKeyMask` and always performs a linear row `memcpy` when the two pixel sizes match. A tiled/swizzled transfer or a keyed copy therefore returns success with incorrect data. The supported type and color-key values are in the [Vita headers](external/vita-headers/include/psp2/gxm.h#L1045); the vendored [Vita3K code](external/Vita3K/vita3k/renderer/src/transfer.cpp#L40) shows the different address and filtering paths.

**Suggested fix:** implement the requested source/destination layouts and key modes, or return an explicit unsupported error until implemented. Test linear-to-swizzled and both keyed modes using a small known pattern.

## 20. HTTP accepts a truncated response body as a successful download — P1

**Location:** [SceHttp.c](source/modules/SceHttp.c#L621), [body parsing](source/modules/SceHttp.c#L722).

The client reads until EOF, but when `Content-Length` is present it only clamps a body that is *longer* than declared. If the connection closes early, it retains the short body, records the declared size, and returns success. A caller that reads until EOF can save an incomplete asset or update as if the download completed normally.

**Reproduced locally:** a server declared `Content-Length: 10`, sent `abc`, and closed. `sceHttpSendRequest` returned 0; subsequent reads returned 3 bytes and then ordinary EOF.

**Suggested fix:** validate framing before declaring success, including premature EOF for fixed-length and chunked bodies, and propagate a network/protocol error to the guest. Add a server fixture that closes mid-body.
