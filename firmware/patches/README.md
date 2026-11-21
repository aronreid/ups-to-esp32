# Patches to ESP-IDF

The firmware is built with ESP-IDF 5.5.5. One IDF component is carried in this
tree with changes, and each change is kept here as a patch so it is easy to read
and to apply again on a newer IDF. The patches apply in this order:
`usb-ep0-abort.patch`, then `usb-stray-chan-guard.patch`.

## usb-ep0-abort.patch

`firmware/components/usb` is IDF 5.5.5's `usb` component (the USB host stack)
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

## Checking the copy

`tools/test/test_usb_patch.py` checks that the copy is exactly IDF 5.5.5's
component plus these patches (the component's test apps and maintainers file are
left out). The component's `CMakeLists.txt` refuses to build on any other IDF
version.

### Moving to a newer IDF

1. Copy the new IDF's `components/usb` over `firmware/components/usb`, leaving
   out `test_apps`, `host_test` and `maintainers.md`.
2. Apply each patch, in order: `patch -p2 -d firmware/components/usb -i firmware/patches/usb-ep0-abort.patch`,
   then the same with `usb-stray-chan-guard.patch`, and fix any part that does not apply.
3. Change the version in the component's `CMakeLists.txt` check and in
   `tools/test/test_usb_patch.py`.
4. Regenerate each patch from the copy before it and the copy after it
   (`diff -ruN a/usb b/usb`), and run the test.

If IDF ever gains a public way to abandon a control transfer, drop that patch and
use that instead. If IDF fixes the stray channel interrupt, drop that one. With
neither patch left, drop the copy.
