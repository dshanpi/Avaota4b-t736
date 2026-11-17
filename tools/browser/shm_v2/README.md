# Cedar ordinary-RAM transport v2: frozen candidate and runtime notes

This isolated candidate keeps `t736_client_*` and `struct t736_frame` unchanged.
The existing v1 core sources remain separate. Runtime scripts and device rules
in this directory were added after the frozen transport archive was built.
The broker and client must be deployed as a matching pair before a new RDD exec;
neither endpoint negotiates down to v1. Setup failure leaves the client unavailable
so the existing first-frame software fallback can run. Errors after hardware
output retain the existing explicit failure behavior.

At preload construction, the client resolves the dedicated
`t736-cedar` service account, checks its runtime directory/socket ownership and
checks the connected peer with `SO_PEERCRED`. Setup then receives exactly one
ordinary memfd using `SCM_RIGHTS` and `MSG_CMSG_CLOEXEC`. It validates the fixed
3,110,400-byte size, regular anonymous file identity/owner, format, seals and
metadata before making the client available. The fd is closed after a read-only
shared mapping is established. The mapping is marked `MADV_DONTFORK`; forked
children cannot use the inherited connection through the client API.

Construction does not guarantee that the process is still in the initial user
namespace. This is the current browser blocker: the real Firefox RDD has the
single UID mapping `1000 1000 1`; the account database returns service UID 999,
while `lstat("/run/t736-cedar")` returns projected owner 65534. The unmodified v2
client rejects that mismatch at `directory.reject` and Firefox uses its existing
software fallback. Disabling the forkserver does not remove this UID projection.
Do not accept 65534 as proof of UID 999 or weaken the browser sandbox.

Each peer has its own memfd. The broker creates its RW mapping before adding
`F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_FUTURE_WRITE | F_SEAL_SEAL`.
`MFD_NOEXEC_SEAL` additionally prevents adding file execution permission; this
does not claim that every possible executable mapping is prohibited. The
candidate requires Linux support for it and fails closed without downgrading.
The receiver cannot write, resize, hole-punch, create a writable shared mapping
or upgrade the read-only mapping with `mprotect`. The broker's existing mapping
remains writable. No DMA buffer, physical address or device descriptor is sent.

PULL returns only a fixed picture descriptor on the socket. The broker completes
its ordinary-RAM pixel copy before sending that descriptor. The client keeps its
existing API mutex held while it validates metadata and copies into the caller's
existing destination. Only then can another request be sent, which acknowledges
consumption of the previous slot. AVFrame ownership and lifetime do not change.
Disconnect unmaps and closes that peer's resources; an old live mapping is never
assigned to a different peer.

This still copies pixels twice after the backend output: backend RAM to shared
RAM, then shared RAM to the existing destination. It removes the large pixel
payload from the Unix stream; it is not a zero-copy path or a demonstrated fix
for Firefox VideoSink lateness. Native transport and browser presentation remain
separate acceptance items.

## Observed results and unfinished work, 2026-11-17

| Test | Result and boundary |
| --- | --- |
| Historical Bilibili hardware baseline | Protocol v1 / FFmpeg v6 / backend v4 / OMX-WC v5 on v27: about 30 fps, 926 total and 0 dropped over 30.863 s; VE IRQ +929 over 30.958 s. This is the verified bounded website hardware result. |
| v29 native RAM protection tests | 58 seal/helper cases PASS with the 200 ms test deadline; actual-client PID, inherited mutex, DONTFORK, address reuse, exec/CLOEXEC and fd-balance tests PASS. |
| v29 native v2 hardware replay | 180/180 frame MD5 and PTS exact, first frame 1.286 s, close=0, after repairing device permissions. |
| v29 native transport without hashing | 540 frames in 6.602932 s = 81.7819 frames/s, IRQ +540, all PTS exact, close=0. Includes backend/client copies, excludes Firefox and display; it is not browser hardware presentation at 81.8 fps. |
| v29 Firefox 1080p60 trial | 1233 presented, 0 dropped in 20.548 s, IRQ +0, RDD about 149.5% of one CPU. This is successful software fallback, not browser hardware decoding. |
| Firefox namespace diagnosis | The isolated diagnostic client confirms `account.uid=999`, projected directory owner 65534 and `directory.reject` before connecting. |

The planned **client v3, still using wire protocol 2**, was stopped at design
and negative-test planning when the user requested final organization/sync.
No v3 implementation directory, binary or completed test suite was produced.
The design discussion about strict maps, trusted pinned `/run` paths and peer
pidfds is not an implemented fix or an authentication claim. A future change
must preserve endpoint authentication; overflow UIDs alone are insufficient.

The completed diagnostic source snapshot, provenance and reproduction commands
are archived in `../diagnostic_client_v1/`. Its original build remains in
`../../../build/cedar-shm-client-diag-v1-20261117/`. It adds bounded stage/return
code/errno and selected numeric namespace metadata logs, retains every v2 guard,
and passed native ASan/UBSan/LSan helper/fork tests and independent source review.
It is for diagnosis, not normal launch or performance measurement.

## Runtime prerequisites and frozen artifacts

The native v28 helper test failed because `CONFIG_ADVISE_SYSCALLS` was disabled
and `madvise(MADV_DONTFORK)` returned `ENOSYS`. v29 enables that option while
retaining the v28 VE400/read-only DCDC2 work. The v28 failure record is retained;
production mapping protection was not bypassed.

A separate cold-boot failure was device access: both nodes returned to
`root:root 0600`, `/dev/cedar_dev` open returned `EACCES`, and vendor OMX crashed
on a null pointer. Protocol v1 reproduced it too. The exact rules in
`70-t736-cedar.rules` assign only `misc/cedar_dev` and `dma_heap/t736-ve` to the
dedicated service owner/group with mode 0600. `cedar-service` now rejects absent,
non-character or unreadable/unwritable nodes before loading vendor libraries.
These are CPU-side access controls, not proof of IOMMU DMA isolation.

The separate runtime prefix is `/opt/t736-cedar-transport-v2`. Its scripts use
backend v5 at `/opt/t736-cedar-browser-backend-v5`, OMX/WC v5, and FFmpeg v7.
Install `config/gstomx.conf` and a private `config/cedarc.conf` preserving other
existing keys; `[paramter] cdc_log_level = 6` suppresses warning-level chatter.
The service remains restricted to v29, uses the dedicated UID, and has no
automatic enable/restart step. Keep the accepted protocol v1 path for rollback.

Frozen archive:
`../../../build/cedar-shm-transport-v2-20261117/t736-cedar-transport-v2.tar.gz`
(49,037 bytes), SHA256
`b1524d0072ad96995c7104ca92c5b9ee46d27bc2990195dbaad629f9796bc92c`.
It contains `bin/cedar-broker`, `lib/libt736_cedar_client.so` and the original
sources/tests/manifest. It does not contain these later runtime scripts/rules,
runtime configuration, backend/vendor/FFmpeg libraries or native test binaries.
The required backend library SHA256 is
`52bf715c24ac9c072c1550f04fe47ab4df195ffdf1b51c4ac7f84414556f8f08`.

The archive carries build UID/GID 1000 and group-writable build modes. Root
deployment must extract with `--no-same-owner` and normalize root ownership,
directories/executables to 0755 and libraries/configuration to 0644. Do not let
the desktop user own libraries loaded by the service. The archive and completed
build record remain unchanged; this working README is a later status document,
so its current hash intentionally differs from the README inside the archive.

Build with `../../build_cedar_shm_transport.py` into a new work directory. The
package contains only the candidate broker/client and their sources/tests; it
requires the separately verified backend v5. The builder performs no service
update, automatic installation, reboot or default-selection step. Do not rerun
it over a completed/frozen work directory.

The builder runs the actual client/broker transport, seals/helper and fork/exec
tests natively with ASan/UBSan/LSan. The seals test also runs with its production
five-second deadline. The ARM64 transport test runs under QEMU; the unmodified
ARM64 seals and fork/exec tests require a native ARM64 kernel. QEMU 8.2.2
linux-user reports successful `MADV_DONTFORK` but retains that mapping in the
fork child, reproduced with a separate program containing no Cedar code. This
limitation is recorded explicitly; it is not a passed ARM64 fork test or a
reason to weaken production mapping protection. Native v29 results above are
recorded separately from this emulator limitation.

## Evidence

- `../../../evidence/cedar-shm-v2-build-review-20261117.json`
- `../../../evidence/cedar-shm-v2-runtime-review-20261117.json`
- `../../../evidence/cedar-shm-v2-client-diagnostic-build-20261117.json`
- `../../../evidence/cedar-shm-qemu-dontfork-limitation-20261117.json`
- `../../../evidence/browser-v29-shm2-hardware-phase-20261117.json`
- `../../../build/cedar-shm-transport-v2-20261117/replay180-v29-PASS.json`
- `../../../build/cedar-shm-transport-v2-20261117/native-transport540-v29-PASS.json`
- `../../../build/cedar-shm-transport-v2-20261117/browser60-v29-software-fallback.json`
- `../../../build/cedar-shm-client-diag-v1-20261117/board-firefox-constructor.log`

Historical evidence/manifest statuses describe their own snapshots. The phase
record and final project status maintained by the board owner supply subsequent
results; a historical `BOARD_PENDING` field is not rewritten into a fabricated
earlier pass. No browser hardware 60 fps or long-term stability pass is claimed.
