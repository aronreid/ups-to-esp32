# Security

If you find a security problem in the firmware or the web page, email
hello@myupspal.com instead of opening a public issue. Say which board and
which version, and what you saw. You'll get a reply, and a fix goes out as an
ordinary firmware release.

The login on a board is plain HTTP, which keeps people out of the settings
rather than encrypting anything. That is documented in
[docs/firmware.md](docs/firmware.md#the-login) and isn't a vulnerability
report on its own. Rev B can also serve its page over HTTPS.

Updates are signed. The images in each release carry a signature made with the
project's key, and a board running one checks the signature of an update before
it installs it. An update that is unsigned, signed with another key or changed
after signing is refused, and the board keeps running what it has. A board
running a build from source without the signing step accepts updates by name only,
as before. The [README](README.md#signed-updates) says what this means if you build
your own firmware.

Physical access is not defended against. A board's flash is not encrypted and its
boot is not signed, so someone with the board and a USB cable can read the Wi-Fi
password and any Tailscale key stored on it, and can load other firmware. The
[README](README.md#security-if-someone-has-your-board-in-their-hands) says what to
do about that. It isn't a vulnerability report on its own.

Only the latest release is supported.
