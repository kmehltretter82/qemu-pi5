Pi 400 / Pi 500 Wi-Fi work-in-progress checkpoint
=================================================

Branch: ``wifi-next-20261017`` in both downstream forks. Development was
paused on 2026-10-17. This is an AI-assisted downstream checkpoint, not a
QEMU upstream contribution and not a qualified release.

Pi 500 integration
------------------

The branch starts from ``pi5-dma-tests-20261010`` at
``68c5213ce84dc1f224c191a30ae088976dba421e``. The first commit restores
the exact source closure of the retained 2026-10-15 private Wi-Fi endpoint.
That predecessor had passed 125 native cases and a four-cell BASE/PT
PREEMPT_RT by firmware-flow-control-mode matrix. Those old results do not
qualify the subsequent changes on this branch.

The endpoint is opt-in with ``-machine raspi500,sdio-wifi=on`` (also
``raspi5b`` and ``raspi500p``). Wi-Fi has its own SDIO2 host; storage stays
on SDIO1. A backend can be selected with
``-nic user,model=cyw43455-sdio``. The imported model includes bounded open
station/AP operation, high-address ADMA64 transport, RX aggregation, active
FWS completion/credit handling, consistent nmode reporting and ordered
interface close/open. It does not execute proprietary firmware or model RF.

The next change imports Pi 400 main's firmware upload/readback RAM, country
state, bounded channel-36/20-MHz station profile and frame-boundary scan
timers. The Pi 500 private migration layout remains independent: versions
29/30 retain their existing identity/AP field order; new state is appended
in version 31. Pi 400 retains its own version-33 layout. Cross-fork migration
is not supported. Active FWS state still explicitly blocks migration.

New shared regressions
-----------------------

``tests/qtest/cyw43455-radio-test.c`` is identical in both forks and uses
each supported board's actual Wi-Fi host. It covers RAM bank/window edges,
out-of-range access and reset, country readback and invalid-write isolation,
2.4/5-GHz scan IE/rate bytes, delayed ACKs and partial FIFO reads, and
migration with a partially elapsed scan timer plus RAM/country state.

The Pi 500 test additionally registers an older-producer migration case
when ``QTEST_QEMU_BINARY_LEGACY`` is set. The retained v30 producer has SHA-256
``76b976a4341c2ba8de9452518b3cedd941540d07d8070db0a8744526af12cc75``.
This case has not yet executed against the new Pi 500 build.

Actual current validation
-------------------------

* Pi 400: a fresh macOS build completed, including all selected qtest
  executables. The new radio suite then failed in
  ``/aarch64/cyw43455-radio/raspi400/5g-pending-scan-migration`` because the
  expected frame-pending bit was absent after the post-migration clock step.
  Its first four cases passed, but that prefix is not a passing suite. The
  test exited with SIGABRT, and its owned process group stopped cleanly.
  The cause remains undiagnosed; no model-versus-oracle conclusion is made.
* Pi 500: the first build reached final linking but exposed a missing SPDM
  backend in the newly added minimal device configuration. The configuration
  now explicitly includes it. The follow-up incremental build was stopped
  on request (exit 130), before a completed candidate binary or test run.
* No fresh Linux guest, full native qualification or physical board test
  was performed for this checkpoint. Prior sealed results remain unchanged.

From a completed build directory, the shared suite can be run with::

  QTEST_QEMU_BINARY="$PWD/qemu-system-aarch64" \
    tests/qtest/cyw43455-radio-test --tap

When work resumes, finish the Pi 500 build, diagnose the pending-scan
migration failure without weakening its intended contract, then run all
unchanged board/AP/FWS suites and fresh Linux driver coverage. Broader
firmware/RF/WPA and non-RT qualification remain separate unfinished work.
