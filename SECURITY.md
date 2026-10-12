# Security

If you find a security problem in the firmware or the web page, email
hello@myupspal.com instead of opening a public issue. Say which board and
which version, and what you saw. You'll get a reply, and a fix goes out as an
ordinary firmware release.

The login on a board is plain HTTP, which keeps people out of the settings
rather than encrypting anything. That is documented in
[docs/firmware.md](docs/firmware.md#the-login) and isn't a vulnerability
report on its own. Rev B can also serve its page over HTTPS.

Only the latest release is supported.
