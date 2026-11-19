# Patches to ESP-IDF

The firmware is built with ESP-IDF 5.5.5. One IDF component is carried in this
tree with a change, and the change is kept here as a patch so it is easy to read
and to apply again on a newer IDF.

## usb-ep0-abort.patch

`firmware/components/usb` is IDF 5.5.5's `usb` component (the USB host stack)
with this patch applied. A component in the project with the same name as an
IDF component replaces it, so this copy is the one every board runs.

The patch adds one function, `usb_host_transfer_abort_control()`. Some UPSes
stop answering one control request while still sending their interrupt reports.
IDF 5.5 gives an application no way to give up on that request: the control
endpoint stays busy, nothing else can be sent, and the UPS reads as stale until
it is unplugged. The new function abandons the request the same way the stack
already does after a control endpoint error. The request comes back to its
callback marked as cancelled, and the next control transfer starts afresh.

`tools/test/test_usb_patch.py` checks that the copy is exactly IDF 5.5.5's
component plus this patch (the component's test apps and maintainers file are
left out). The component's `CMakeLists.txt` refuses to build on any other IDF
version.

### Moving to a newer IDF

1. Copy the new IDF's `components/usb` over `firmware/components/usb`, leaving
   out `test_apps`, `host_test` and `maintainers.md`.
2. Apply the patch: `patch -p2 -d firmware/components/usb -i firmware/patches/usb-ep0-abort.patch`,
   and fix any part that does not apply.
3. Change the version in the component's `CMakeLists.txt` check and in
   `tools/test/test_usb_patch.py`.
4. Regenerate the patch from a clean copy and the patched one
   (`diff -ruN a/usb b/usb`), and run the test.

If IDF ever gains a public way to abandon a control transfer, drop the copy and
the patch and use that instead.
