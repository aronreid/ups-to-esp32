# Patches to ESP-IDF

The firmware is built with ESP-IDF 5.5.6. Two IDF components are built with
changes, and each change is kept here as a patch so it is easy to read and to
apply again on a newer IDF.

- The USB host stack (`usb-*.patch`, below), a whole copy of the component in
  `firmware/components/usb`. The patches apply in this order:
  `usb-ep0-abort.patch`, then `usb-stray-chan-guard.patch`, then
  `usb-halt-error-wake.patch`.
- lwIP, the network stack (`lwip-*.patch`, see *lwIP* at the end): only the two
  changed source files are carried, in `firmware/lwip-patched`.

## usb-ep0-abort.patch

`firmware/components/usb` is IDF 5.5.6's `usb` component (the USB host stack)
with the patches below applied. A component in the project with the same name as an
IDF component replaces it, so this copy is the one every board runs.

The patch adds one function, `usb_host_transfer_abort_control()`. Some UPSes
stop answering one control request while still sending their interrupt reports.
IDF 5.5 gives an application no way to give up on that request: the control
endpoint stays busy, nothing else can be sent, and the UPS reads as stale until
it is unplugged. The new function abandons the request the same way the stack
already does after a control endpoint error. The request comes back to its
callback marked as cancelled, and the next control transfer starts afresh.

## usb-stray-chan-guard.patch

The host controller driver keeps three 2-bit counters per pipe for its two
transfer buffers (to fill, to execute, to parse). Every channel interrupt it
handles marks the executing buffer done and then parses it. One that arrives
when no buffer is executing (for example late, after the port went down and a
flush had already retired the buffer) marks a buffer done that holds no
transfer: parsing it trips `assert(buffer_to_parse->urb != NULL)` inside the
USB interrupt handler, and "to execute" wraps below zero. Either way the board
restarts.

The patch makes the channel interrupt handler ignore such a stray interrupt and
count it, still waking a task that is waiting for a halt.
`usb_host_stray_chan_events()` returns the count; the board reports it on the
Debug card and in its diagnostics.

## usb-halt-error-wake.patch

To stop a pipe, the driver asks the channel to halt and waits for the
interrupt that says it has. If the channel ends with an error at the same
moment (a STALL, a transaction error, babble), the hardware layer reports the
error, which takes precedence, and leaves its "halt requested" flag set. The
driver's error handling never woke the waiting task, and as the channel is
already halted no other interrupt comes: the task waits for ever. For this
board that task is the one reading the UPS (stopping the interrupt pipe) or
the USB library task (abandoning a control request).

The patch wakes the waiting task on an error too, and clears the stale
"halt requested" flag, which would otherwise make the next normal completion
on that channel read as a halt with no one waiting.

## Checking the copy

`tools/test/test_usb_patch.py` checks that the copy is exactly IDF 5.5.6's
component plus these patches (the component's test apps and maintainers file are
left out). The component's `CMakeLists.txt` refuses to build on any other IDF
version.

### Moving to a newer IDF

1. Copy the new IDF's `components/usb` over `firmware/components/usb`, leaving
   out `test_apps`, `host_test` and `maintainers.md`.
2. Apply each patch, in order: `patch -p2 -d firmware/components/usb -i firmware/patches/usb-ep0-abort.patch`,
   then the same with `usb-stray-chan-guard.patch` and
   `usb-halt-error-wake.patch`, and fix any part that does not apply.
3. Change the version in the component's `CMakeLists.txt` check and in
   `tools/test/test_usb_patch.py`.
4. Regenerate each patch from the copy before it and the copy after it
   (`diff -ruN a/usb b/usb`), and run the test.

If IDF ever gains a public way to abandon a control transfer, drop that patch and
use that instead. If IDF fixes the stray channel interrupt, drop that one; if it wakes a halt
waiter on a channel error, drop the third. With
no patch left, drop the copy.

# lwIP

Four fixes from upstream lwIP (https://github.com/lwip-tcpip/lwip) that no
ESP-IDF release carries yet. Each patch file starts with what it fixes and the
upstream commit. They apply in this order, to IDF 5.5.6's lwip component (the
paths are `a/lwip/<path in the component>`, so `patch -p2 -d <lwip component>`):

1. `lwip-tcp-fin-copy-window.patch` (upstream 777e6d40d): a FIN taken from the
   out-of-order queue after the segment was trimmed to the receive window
   underflowed the window, an assert and restart that any TCP peer could cause.
2. `lwip-tcp-optbyte-bound.patch` (3d896ba0a): reading a TCP option's length
   byte at the end of the options read past them.
3. `lwip-tcp-syn-rcvd-seqno.patch` (6a3a94420, less a whitespace-only hunk):
   the ACK that completes a handshake is checked against the receive window.
4. `lwip-nd6-ptb-min-mtu.patch` (d08f4773e and 3d1c9bff5): an ICMPv6 Packet Too
   Big below IPv6's minimum MTU of 1280 is dropped. A forged one could make
   IPv6 fragmentation loop for ever on the network thread.

Upstream 3b94c1abb (trim an out-of-window segment before queuing it out of
order) is not carried: IDF's lwIP trims such a segment when it leaves the
queue (its `ESP_LWIP` block in `tcp_receive()`), which already keeps the
window from underflowing.

Only the changed files are carried, not the component: `firmware/lwip-patched`
holds IDF 5.5.6's `tcp_in.c` and `nd6.c` with the patches applied, and
`lwip-patched.cmake` (included from `firmware/CMakeLists.txt` after
`project()`) swaps them into IDF's own lwip library in place of the originals.
Same library, compile options, include paths and object names, so nothing else
about the build changes. It pins each IDF original by SHA-256 and stops the
build on any other IDF.

`tools/test/test_lwip_patch.py` checks that the copies are exactly IDF 5.5.6's
files plus these patches, that the pins match, that the fixed lines are there,
and that each firmware build directory compiles the copies.

### Moving to a newer IDF

1. Check which of the four the new IDF's lwIP already has, and drop those
   patches (and from the list in `tools/test/test_lwip_patch.py`).
2. Copy the new IDF's `components/lwip/lwip/src/core/tcp_in.c` and
   `components/lwip/lwip/src/core/ipv6/nd6.c` into `firmware/lwip-patched`
   (same paths), and apply the remaining patches in order with
   `patch -p2 -d firmware/lwip-patched -i firmware/patches/<patch>`.
3. Put the new originals' SHA-256 in `lwip-patched.cmake`, change the version
   in the test, regenerate the patches from the files before and after, and run
   the test. With no patch left, delete `firmware/lwip-patched` and its
   `include()`.
