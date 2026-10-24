# Security

## Reporting a problem

If you find a security problem in the firmware or the web page, email
hello@myupspal.com instead of opening a public issue. Say which board and
which version, and what you saw. You will get a reply, and a fix goes out as an
ordinary firmware release. Only the latest release is supported.

## What protects the board on your network

- **The login.** Every setting and every UPS command, on the page and over NUT, needs
  it. Reading the UPS never does. On Rev A the page is plain HTTP, so the login keeps
  people out of the settings rather than encrypting anything; Rev B can also serve the
  page over HTTPS. See [docs/firmware.md](docs/firmware.md#the-login).
- **Signed updates.** Release images carry a signature made with the project's key, and
  a board running a release checks an update's signature before installing it. An
  update that is unsigned, signed with another key or changed after signing is refused,
  and the board keeps running what it has. A build from source without the signing step
  accepts updates by name only. See [docs/updates.md](docs/updates.md#signed-updates).
- **Failed logins are rate-limited** per device, so a stray script cannot lock you out.
- **Nothing answers on the setup access point** except the setup page itself.

A board with no login set can be claimed by the first device on your network that asks.
That is the state of a board after the serial `clear-login` command, and of one updated
from a release that had no login. Set the login right after you update or clear it.

## If someone has your board in their hands

The login and HTTPS protect the board from other people on your network. They do not
protect it from someone who has the board and a USB cable. The board does not encrypt
its flash memory or require signed firmware at boot, so anyone who plugs in a cable can
read what it stores:

- your Wi-Fi password, in plain text
- the login's password hash (salted, but a short password can be guessed offline)
- on Rev B, the Tailscale key, and the keys behind the board's own HTTPS certificate

They can also load their own firmware onto it.

What to do about it:

- Treat the board like your router. Keep it where only people you trust can reach its
  USB-C port.
- If a board is lost, stolen or given away, change your Wi-Fi password. Press BOOT five
  times first if you still have it, which clears the Wi-Fi details, the login, the HTTPS
  keys and, on Rev B, the Tailscale identity.
- On Rev B, create a Tailscale key that is single-use or expires, and remove the board
  from your Tailscale admin page when it leaves your hands. A reusable key stored on a
  lost board would let someone join your tailnet.
- Use a login password you do not use anywhere else.

This is a choice. The firmware is open source, and the boards are meant to be yours: you
can reflash one with your own build, or with someone else's, whenever you like.
Encrypted flash and signed boot would make that harder, because the setting is permanent
on a board and ties it to one signing key. The cost is the exposure described above, and
we would rather say so than hide it.

If you want locked-down boards, or you have a better way to offer both, open an issue or
email hello@myupspal.com. If enough people ask, a locked-down option may come in a later
hardware batch, and this page will say so.
