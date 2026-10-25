# Changelog

What changed in each release, newest first. Changes that are not yet in a
release are listed under Unreleased.

## Unreleased

(nothing yet)

## v0.17 -- 2026-10-25

- APC Smart-UPS units that report only some readings over USB (such as the Smart-UPS X 1500 without Modbus) now show what they do report: battery voltage appears on the page, and the overview fills its row with the readings the UPS has (battery voltage, output, frequency) instead of showing runtime alone.
- Wi-Fi: a board that had opened its rescue setup page could loop through setup: join the network, page goes away, setup page again, until it was power cycled. A successful join from the setup page now clears the count that opens rescue, so the board restarts onto the network it was given.
- A UPS (or anything pretending to be one) can no longer stall the board with a hostile USB report descriptor (Report Count is capped at 255 per item, as NUT does), make the interface scan read past the end of a truncated configuration descriptor, or hit undefined float-to-integer conversions with an out-of-range reading.
- NUT: a UPS whose model name contained text like `VAR ups ups.status` could answer a status request with its own text. Variables are now found at the start of a line only. An unknown NUT command no longer counts as proof that a freshly installed update works; only a LIST VAR or GET VAR reply does.
- Rev B: a network device that keeps changing the board's address can no longer make it re-issue its HTTPS certificate and restart HTTPS every ten minutes, and each replaced certificate no longer stays in memory for good. An address change re-issues at most once an hour; a certificate near its end is renewed at once.
- Updates: the download address in a release is now accepted only if it is an https address on GitHub. (The HTTP client already refuses to follow a redirect from https to plain http.)
- Rev B: a spoofed internet-time reply can no longer make the board issue its HTTPS certificate with dates decades in the future. A clock more than 20 years past the firmware's build date is not used.
- Pressing BOOT five times now also clears the Wi-Fi driver's own saved copy of the network name and password (the board could rejoin the old network after a reset), and on Rev B erases the outage log and the long history, so a board passed on keeps nothing of its previous owner. The reset takes about a minute on Rev B while the log flash is erased.
- Rev B with Tailscale: the auth key you enter is now erased from the board as soon as it has joined your tailnet; the board keeps running on the identity it was given. The page then reads "registered: no key is kept". You only enter a key again if the tailnet stops accepting the board's key (for example, key expiry was left on).
- Rev B with Tailscale: peer keys in the netmap are now checked to be the right length (a malformed one skips that peer), the temporary handshake key is erased once the handshake is done, a forged first-message frame stops the handshake instead of being skipped, and the saved peer list is checked against its own size when read back.
- Rev B with Tailscale: a replayed captured WireGuard packet can no longer move a peer's address or refresh its liveness, and a malformed short tunnel packet from a peer is dropped before any of it is read.
- Changing the login on the page now asks for the current password as well as being logged in, so a stolen or left-behind browser session cannot take the board away from its owner.
- A board with no login yet now answers only to its own names (its address, its hostname, `<hostname>.local`, the setup address, its Tailscale address). A web page on the internet that tricks your browser into reaching the board under another name (DNS rebinding) can no longer create the first login on it. Boards with a login are unchanged.
- Wrong NUT passwords from other devices can no longer lock you out of the web page. NUT failures are now counted per device only, and a wrong NUT password ends that connection. Wrong web passwords still trip the board-wide lock.
- A board the fleet server names a test board no longer sends its device details (address on your network, hostname, crash log) on the server's say-so. The project's own test boards are marked at the board itself, once, by typing `bench-on` into the USB-C serial console; only a board marked that way and named a test board by the server sends them. Everyone else sends stats only if they tick share stats.
- Updates: a board now checks that the firmware it downloaded is the release it was published as. An older genuine build re-uploaded under a newer release name is refused, and the page says so. An automatic install must also be newer than the running version; pressing Install yourself still works for rolling back to a real release.
- Rev B with Tailscale: the board no longer accepts subnet routes from other devices on the tailnet. A peer could previously claim any address range (including your home network's) as a source for traffic into the board. Only each peer's own tailnet address is recognised.
- Rev B with Tailscale: the board now asks for a cookie round trip before doing the expensive part of a WireGuard handshake when initiations arrive in a burst (more than four a second), as the protocol intends. Before, that protection was switched off.
- Rev B with Tailscale: a stray UDP packet shaped like a STUN reply can no longer change the public address the board advertises to other devices. A reply is accepted only for a request the board sent.
- Rev B with Tailscale: the key expiry date the Tailscale control server sends is now checked before it is used. An impossible month or year used to crash the board, and the same message arrived again on every reconnect.
- The board's setup Wi-Fi now exposes only what setup needs. `/metrics` returned 404 on that network on paper, but the check never matched (the web server's sockets are dual-stack), so it was readable by anyone in range; it now is not. And on a board with no login yet, only the Wi-Fi form and the first-login form are open on the setup network: commands, updates, Tailscale and network settings are refused until a login exists. The Wi-Fi scan is limited to the setup network until a login exists.
- Rev B with Tailscale: a flood of UDP packets to the Tailscale port could stall the board's network (each packet printed two log lines before any key was checked). Those lines are now silent, and the rest are limited to one every 10 seconds.
- Rev B: a device that opens a connection to the HTTPS port and says nothing no longer keeps the page off. The TLS handshake now has 3 seconds instead of 10.
- A device that connects to the web page and trickles bytes can no longer hold the page for hours. Receive timeouts are now 2 seconds and request headers are capped at 512 bytes. Logging in no longer waits behind a slow request.
- Fixes a NUT client that connects and sends nothing tying up the server. A connection now has 20 seconds to send its first command (the 15 minute idle limit applies after that), and one address can hold at most two connections, so an idle or hostile device can no longer lock Home Assistant and the NAS out of the board.
- Fixes a way for a malicious release name on GitHub to run script in the board's web page. A release tag is now refused unless it looks like one of ours (`v0.17`, `v0.17-rc1`), and the page escapes every name it shows from outside the board. The page also now sets a content security policy and refuses to be framed.
- A slow or stalled client can no longer freeze the web page. The board now gives a request body 4 seconds to arrive in full and answers 408 after that; before, a device sending one byte at a time could hold the page up for as long as it liked.
- Tailscale boards (Rev B): fixes two races in the tunnel code. Handshakes, keepalives and the periodic WireGuard work ran on their own task while lwIP's network thread sent data over the same tunnel, so under load the two could reuse an encryption nonce or read a half-installed key (packets the peer silently dropped), and tearing the tunnel down while a NUT or HTTPS client was connected over it could crash the board. All WireGuard state is now owned by lwIP's network thread, and the tunnel is torn down in an order that cannot free a connection still in use.
- Updates are now signed. Release images carry a signature made with the project's key, and a board running one checks the next update's signature before it installs it. An update that is unsigned, signed with another key or altered is refused, the page says so, and the board keeps running what it has. Nothing is burned into the chip: a board can still be reflashed over its USB cable with any build. A build from source without the signing overlay works as before and checks updates by name only.
- Fixes the daily update check running every four minutes on production boards. The twelve hour wait overflowed a 32-bit tick count and came out as 250 seconds, so every board contacted GitHub that often. It now checks once every twelve hours (three hours on the dev and debug channels).
- A channel change from the fleet server no longer behaves like pressing Check: it no longer clears a refused release, no longer shows a red error if GitHub is briefly unreachable, and no longer replaces "restart to finish" after an install.
- Install is no longer offered while the new firmware is still on trial after an update, and says why if asked, instead of "download would not start".
- A reply from the fleet server that is not a proper answer (an empty or HTML page from a proxy) is now a failed check-in, so it no longer moves a dev board back to the production channel.
- A release with no publish date (a draft) no longer cuts short the search for the newest release on the dev channel.
- Holding BOOT through five presses (factory reset) now also erases the board's HTTPS certificate keys and, on Tailscale boards, its tailnet identity, so a board passed on to someone else keeps nothing of the previous owner's.
- A board whose Wi-Fi stays connected but loses its IP address lease now counts as offline: the page shows it, and the fifteen minute offline restart can step in if it never comes back.
- Fixes a NUT client that never reads its replies tying up the server. A client that kept asking for `LIST VAR` and never took the answers held one of the board's NUT connections for ever; with three such connections no real client could connect. A reply that cannot be delivered for ten seconds now drops that connection.
- A few wrong passwords from one device no longer lock everyone out. Failed logins are now counted per device address, for the web page and NUT alike, so a stray script cannot keep the owner or Home Assistant out of the controls. A much higher board-wide count still stops a guesser using many addresses.
- Fixes setting up Wi-Fi with a mistake in the form. If the network name was missing, the board still saved the login from the same form, so the corrected retry was ignored and the typed password stuck. The form is now checked as a whole first, and a join that cannot start undoes the login it just made.
- Fixes requests from the web page being refused as "not JSON" when the data arrived in more than one piece (more likely over HTTPS or Tailscale), and a few failures that left the page waiting with no answer.
- A dev or debug board no longer installs a release by itself when the fleet server names one that is not newer than the running version. It is still offered on the page, and pressing Install still works, so a person can roll back. A tag the board cannot read as a version (a release candidate counts as older than its release) is never installed automatically.
- NUT and `/metrics` no longer answer on the board's own setup Wi-Fi. That network is open, and it comes up during first-time setup and after repeated offline restarts, so anyone in radio range could read the UPS. NUT now refuses those connections and `/metrics` returns 404 there. The setup page and the Wi-Fi and login forms work as before, and nothing changes on your home network.
- Fixes the Wi-Fi network list breaking when a network name is full of quotes or backslashes.
- Fixes HTTPS staying off until a restart after it failed to start at boot. The board now tries again every ten minutes.
- NUT protocol tidying: names a client sends are echoed back quoted, `LIST CLIENT` with a wrong UPS name answers `ERR UNKNOWN-UPS`, `GET VAR ups` answers `ERR INVALID-ARGUMENT`, and `HELP` lists `STARTTLS` only on boards that have it.
- Rev B: the outage log copes with a flash write that fails. A write that failed without writing anything used to be skipped over, leaving a gap that could make the next restart erase the newest records or lose the end of an open outage; the record now goes into the same place once the flash takes it, and a log with such a gap from an earlier version is read correctly. A flash that keeps failing is now tried less and less often (up to every five minutes) and stops after a few bad records until the board restarts, instead of moving on twice a second and erasing old history every minute. The page's count of records that could not be written still shows it.
- Rev B: an outage that goes on through a restart of the board now gets its full length in the outage log. The board's first record after the restart was written before it had the time from the internet, so the time the board was down was never counted and the outage showed as having a gap. That record now waits for the clock (up to 90 seconds) and keeps the time it was taken.
- Rev B: a board whose settings storage would not open or would not save no longer mixes up its restarts. Each restart is numbered in the settings storage, and when that failed every restart got the same number 0, so the outage log could add up time across restarts that never happened and the history chart could put readings from days ago at the wrong time. A restart whose number was not saved is now treated as unknown, and the chart tells this restart's readings apart without it.
- Rev B: the outage list no longer shows outages whose records the log has already overwritten to make room (it now matches what a restart would show), and its count of records no longer grows past the log's size. When the board is briefly short of memory, the page keeps the list it has and asks again, instead of showing that no outages were recorded.
- Fixes a rare case where the board could write to flash (history, the outage log, an update) while the UPS's USB was still active, the condition that crashed v0.14.4 during updates. A command from the page or a NUT client that timed out while the UPS was slow left its answer behind, and the next request (a pause of USB before a flash write) could take that answer as its own. Every request to the USB side is now numbered and only its own answer is accepted.
- Fixes a UPS that stops answering one of its reports still showing that report's last values and status as current. A UPS that went deaf on its status report while its other reports kept answering could report "on mains, 100 %" through a power cut, and NUT never went stale. A value whose report has not answered for about ten seconds is now dropped, and a missed status read counts as a failed poll, so a status that stays unreadable makes NUT report the data as stale and the board power-cycles the UPS's USB link as it does for a UPS that answers nothing.
- Fixes a Rev A board that could stop reading its UPS for good until it was power-cycled by hand. v0.16's restart for a stuck USB side only noticed it when history or the outage log paused USB, and those exist only on Rev B; on Rev A the readings just went stale. Every board now watches the USB task directly and restarts itself if it has not run for a minute.
- A UPS that answers very slowly no longer makes the board restart for nothing. Each slow read waited 2 seconds for the one before, and one round of reads could add up to nearly a minute, close enough to the one-minute "the USB side is stuck" limit to trigger a restart while it was still working. After the first slow read in a round the rest give up at once, a round's reads stop after 30 seconds, and the next round tries again.
- Fixes a board that could keep showing an unplugged UPS as attached, with its last readings frozen. If the USB driver had not finished with the UPS's last transfer when it was unplugged, the board waited for it with no limit. The readings now go as soon as the UPS leaves, and if the driver has not let go after a minute the board restarts.
- Fixes NUT variables such as `ups.mfr` or `battery.type` staying missing until the UPS was replugged when the first attempt to read them failed. A fixed value is now read again until it has actually been read once.
- Fixes NUT reporting `DATA-STALE` instead of `OB` for some UPSes on battery (OpenUPS, and Liebert or MGE-protocol units whose only mains indicator says "not on mains"). NUT reports `OB` whenever a UPS's own mains indicator is off; the board only did when a separate "on battery" or "discharging" indicator was also set. It now follows NUT.
- PowerWalker (Phoenixtec, USB ID 06da:ffff) UPSes are now read with NUT's Liebert/Phoenixtec table, the one NUT itself uses for them, instead of the Eaton/MGE table. The MGE table misread one of their indicators, so a PowerWalker VI 3000 RT on mains showed as on battery (`OB`) where NUT says `OL CHRG`, and it could show variables NUT does not. AEG PROTECT units under the same ID keep the MGE table, and GoldenMate battery packs get the iDowell one, as in NUT.
- The report descriptor download (`/api/descriptor`, and the copy sent with the heartbeat) is only offered once the UPS's descriptor has been read in full. Asked for while a UPS was being replugged, it could mix the old UPS's bytes with the new one's; a legacy Tripp Lite, which has no descriptor of this kind, no longer offers one.
- The README and SECURITY.md now say plainly what someone with the board and a USB cable can read (the Wi-Fi password, the login hash, and on Rev B the Tailscale key and certificate keys) and that flash encryption and signed boot are left off on purpose so boards stay reflashable. They also say what to do if a board is lost or given away, and invite feedback.
- Rev B with Tailscale: fixes a crash when the relay connection drops while a relayed packet is only half received. The reconnect could free the connection while the task reading it was still inside it, and that task could then read the start of the new connection.
- Rev B with Tailscale: the board joins the tailnet up to 25 seconds sooner after each start and reconnect. It used to time every relay region and then ignore the result.
- Rev B with Tailscale: the board no longer writes a console line for every packet through the tunnel. Those lines limited how fast the tunnel could go and held up NUT on the local network too.
- Rev B with Tailscale: the list of tailnet peers is saved to flash once per update from the control server instead of once per peer, and not at all when nothing changed, which saves flash wear.
- Rev B with Tailscale: the board now checks the relay server's certificate.
- Rev B with Tailscale: fixes peers refusing connections the board starts after a restart before it has the time from the internet. The handshake timestamp could go backwards, and a peer would not accept the board's handshakes until the peer itself restarted.
- Rev B with Tailscale: fixes a possible crash when the page asks for the Tailscale status in the first moments after the board starts.

## v0.16 -- 2026-10-12

- Fixes boards not being able to see a new release. A release's information had grown past the 24 KB a board reads, so a board on the dev channel said "the release information is too large to read", and a production board would have said the same once it was published. The release now carries fewer files (the debug ELFs are kept separately), and the release workflow checks the size.
- A new look for the web page, the same on every board. Each UPS is a little battery with a face that shows how it is doing, with a plain-words status, a menu that splits the page into sections, and larger touch targets for a phone. Nothing it shows or controls changed.
- An update no longer power-cycles the UPS's USB link. The board pauses USB while it writes flash, and that pause was taken for the start of a battery test, whose rule is to power-cycle the link afterwards: every update since v0.15.1 dropped and re-enumerated the UPS twice, and NUT clients could see `DATA-STALE` for a few seconds.
- Rev B boards include Tailscale. Enter an auth key on the board's Tailscale card and it joins your tailnet, so NUT clients and your phone reach it from anywhere, with no port forwarding or subnet router. It stays off until a key is entered.
- Fixes a board that could stop reading its UPS for good, with no error, until it was restarted by hand. Before writing to flash (history, the outage log, an update) the board pauses USB traffic, and the USB driver can wait forever while stopping a transfer. The board kept answering on the network but every reading went stale. It now notices when the USB side stops answering for a minute and restarts itself.
- Fixes a board restarting itself whenever Wi-Fi dropped, once it had been up for more than fifteen minutes. The offline watchdog measured time offline from when the board last got an address, usually at boot, so a router reboot or an access point restart turned into a full restart of the board (and every NUT client cut off) instead of a reconnect. It now restarts only after fifteen minutes actually offline, as intended.
- Frees internal memory so the board has more room under load. A Rev B board idles with about 195 KB of internal RAM free (was 116 KB) and its largest free block went from 36 KB to 114 KB: the network and Wi-Fi libraries' static data, the mDNS task and the Wi-Fi receive buffers now live in the 8 MB of PSRAM, and Wi-Fi speed-up code and the TLS peer certificate no longer take internal RAM. A Rev A board gets about 36 KB more (Wi-Fi speed-up code, FreeRTOS, heap and ring buffer code now run from flash, and the data cache is 16 KB instead of 32). Settings only; Wi-Fi throughput drops slightly, which a UPS monitor does not notice.
- The product is now called **My UPS Pal**. The page, NUT's `VER` reply and the board name in `/metrics` (`board="My UPS Pal Rev A"`) say so. Nothing a board or a client depends on changed: the network name stays `ups-esp32-XXXX`, updates come from the same place, and an existing board keeps its HTTPS certificate. A Prometheus query that matched the old board name needs the new one.
- A deep battery test is refused unless the battery reads 100 %. It runs the battery down (an APC RS 1000G went 100 to 22 % in 40 minutes), so starting one on a part-charged battery leaves less reserve than the UPS's own estimate while the mains can still fail. The web page says why; NUT clients get `ERR INSTCMD-FAILED`. A UPS that reports no charge is not blocked.
- Firmware is built with ESP-IDF 5.5.5, the same version for every board. The build refuses any other ESP-IDF line.
- NUT on a board with PSRAM serves up to six clients at once, each on a worker whose memory is in PSRAM; a seventh connection is refused until one leaves. Before, every client took about 12 KB of the chip's internal memory, so a handful of STARTTLS clients (or anyone on the network opening connections) could run it out: the third STARTTLS client on a busy board was dropped with "no memory for a TLS client".
- The parsed UPS report descriptor takes 2 KB less memory: each of its 256 field slots is 32 bytes, down from 40. On a board without PSRAM that is 2 KB more internal memory free.
- On a board with PSRAM, the heartbeat check-in no longer waits for 60 KB of the chip's internal memory to be free ("deferred: memory is busy"). Its TLS session's memory is in PSRAM there, so it now asks for 20 KB, and a busy board checks in on time.
- Smaller task stacks free internal memory on every board: about 5 KB without PSRAM, about 10 KB with it, and 2 KB more with Tailscale on. The BOOT-button reset and the serial `clear-login` command now share one task, and with PSRAM the status lights and the display run from it.
- A board with PSRAM keeps about 31 KB more of the chip's internal memory free. The UPS's parsed report descriptor, NUT's variable table, the web page's reply buffers, the update notes, the outage log and history write queues and the display's frame buffer now live in PSRAM.
- NUT on a board without PSRAM serves up to three clients at once; a fourth connection is refused until one leaves. Each client used to get a task of its own with no limit, so enough connections from anywhere on the network could use up the board's memory. A Home Assistant install and a NAS or two still fit.
- With Tailscale on, a Wi-Fi drop or a Tailscale restart no longer risks rebooting a board whose internal memory is almost used up. Stopping Tailscale's tasks used to need a fresh allocation for each one, and the board restarted if that allocation failed.
- Enclosure: the RESET and BOOT labels on the lid are larger (2.6 mm caps, up from 1.8 mm) so the strokes are wide enough for a 0.4 mm nozzle to print legibly. The words also sit a little further below their pinholes.
- Enclosure: the OLED module clamp is trued up. The pocket between its end walls is 28.0 mm (was 29.3) and centred on the header, so the module centres itself along the display instead of sliding side to side, and the slot is 0.15 mm taller so a 1.2 mm module PCB no longer squeezes in.

## v0.15.1 -- 2026-10-02

- Updates no longer crash with some UPSes attached. A board on v0.14.4 with a
  CyberPower EC850LCD rebooted every time it began downloading v0.15 -- a
  panic in the USB host driver -- so it said it was updating and never did,
  and its crash count rose by one each try. The board now pauses all USB
  traffic to the UPS for the length of an update and resumes it after; the
  UPS's readings hold their last values meanwhile. A board still on v0.14.4
  or v0.15 is running the old update code, so its update to this release can
  still crash. For that one update, press the page's button that power-cycles
  the UPS's USB link, then start the update straight away: with the link
  down, nothing is in flight while the image is written. (Unplugging the
  UPS's USB cable from the board does the same.)

## v0.15 -- 2026-10-02

- NUT no longer reports the last status it saw (for example "OL") once the UPS
  has stopped answering. Clients now get "data stale", as they would from NUT
  itself, so a power cut while the board cannot read the UPS is no longer
  hidden behind an old "on mains".
- A UPS that fails its first detection attempt is tried again, every few
  seconds and then every minute, instead of staying "not detected" until
  someone replugged it. This also covers a UPS still waking up after the board
  power-cycles its USB.
- UPSes with a large report descriptor (over 2048 bytes, such as some Eaton
  and Legrand units) are now detected. So are UPSes whose descriptor is
  shorter than they claim, which NUT accepts too.
- A reply from the UPS that is shorter than expected no longer reads as zeros.
  Before, it could briefly show "on battery" with the mains present.
- Older Tripp Lite UPSes (USB ID 09ae:0001) should now be detected: every
  query to them used to time out. Not yet tried on a real unit.
- APC Smart-UPS units that keep their voltages out of the normal USB data,
  such as the Smart-UPS C 1500, now report input and output voltage, load,
  watts, output current and frequency, battery voltage and temperature. The
  board reads them through APC's Modbus channel on the same USB cable, as
  NUT's `apc_modbus` driver does. Modbus has to be turned on in the UPS's
  front-panel menu first (Configuration, Modbus, Enable). Not yet tried on a
  real unit.
- New download at `/api/modbus`: what the board and an APC Smart-UPS said to
  each other over Modbus (the last 16 exchanges with their raw replies, the
  raw registers, and counts of answers and timeouts). Save it with
  `curl -o modbus.json http://ups-esp32-XXXX.local/api/modbus` and send it in
  if the readings look wrong.
- For an APC Smart-UPS read over Modbus, "Send my UPS info to the developer"
  now sends that Modbus capture along with the descriptor. It includes the
  UPS's readings, and the page says so. With stats ticked, the board also
  sends one capture by itself once per boot.
- A check-in the fleet server answers with an interval of zero no longer counts
  as a failed one. Before, the board backed off and "Send my UPS info" said it
  could not reach the server.
- A self-test or other command sent while the board is waiting on a slow or
  silent Smart-UPS Modbus reply now goes through at once, instead of timing
  out on the page and then running anyway.
- PeaNUT now shows the board's UPS. It asks the type of every variable
  (`GET TYPE`), which the board did not answer, and PeaNUT then listed the
  whole UPS as "Device Unreachable". `LIST ENUM` and `LIST RANGE` are
  answered too.
- If the board cannot join its saved Wi-Fi for half an hour (a mistyped
  password, or a router whose name or password changed), it opens its setup
  network for five minutes so you can put it right, then tries the saved
  network again. It keeps doing that until one works. If the board has a
  login, changing the Wi-Fi from that setup page still asks for it. Before,
  the only way back was pressing BOOT five times.
- A dropped Wi-Fi reconnect attempt is retried rather than waiting for the
  board's fifteen-minute restart.
- NUT clients that vanish without disconnecting (a NAS that lost power, a
  Wi-Fi drop) are noticed within about two minutes, not two hours. Before, a
  few of them could use up the board's connections and lock out new NUT
  clients, the web page and updates.
- NUT commands split across two network packets are now read whole. A client
  that sent many commands at once could get its answers out of order.
- upsmon running as primary no longer logs an alert on every connect: the
  board now answers PRIMARY and MASTER. LOGOUT closes the connection, and
  STARTTLS gets the same answer as NUT without a certificate.
- A UPS name, model or value containing a quote or backslash is now escaped
  for NUT clients instead of breaking the line.
- The status page could crash the board if several long error messages were
  showing at once. It now reports an error instead.
- If the USB host, the NUT server or the update service fails to start, the
  board carries on without it instead of restarting into the same failure. A
  freshly installed update that fails this way still rolls back.

## v0.14.4 -- 2026-09-30

- The board now also listens for the reports a UPS sends the moment
  something changes, as NUT does, so a switch to battery shows up straight
  away rather than at the next poll.
- Liebert UPSes now get ups.status from NUT's own logic too: on mains, on
  battery, charging, low battery, replace battery and shutdown imminent.

## v0.14.3 -- 2026-09-30

- CyberPower UPSes now report battery voltage, with NUT's own corrections:
  some CP-series units (USB ID 0764:0501) report battery voltage 1.5 times
  too high and frequency in tenths of a hertz, and the board now fixes both
  as NUT does. Battery charge is capped at 100%.
- ups.status is now built exactly the way NUT's own driver builds it, from
  NUT's per-make status tables, so it can report TRIM, BOOST, BYPASS and CAL
  as well as on mains, on battery, low battery, overload and replace battery.
  NUT's ups.alarm messages (for example "Replace battery!") and
  input.transfer.reason are published too. A UPS whose table does not give
  its power state keeps the board's previous status logic.
- UPSes that present more than one USB HID interface are now found on the
  right one. Some (the Liebert PSI5, for one) put a different device first,
  and the board used to talk to that instead of the UPS.
- NUT clients now get each value the way NUT itself reads it for your make of
  UPS, where the two differ: for example dates as 2014/02/21.
- A battery voltage far from the battery's own rated voltage (under half, or
  over 1.6 times) is dropped rather than published.

## v0.14.2 -- 2026-09-30

- Many more readings from your UPS, over NUT and on the page ("More from
  this UPS"). The board now uses the same per-make tables NUT itself uses, and
  publishes everything your UPS actually reports: nominal voltages and power,
  battery type and dates, low-battery and runtime warnings, transfer
  thresholds, timers, and on Eaton units efficiency, energy saving and outlet
  status. An APC Back-UPS now gives 27 variables to a NUT client, up from 21.
  Checked against 2,952 values NUT itself decoded from 48 real UPSes: every one
  matches.
- Fixed: on APC UPSes, battery.mfr.date showed the UPS's own manufacture date,
  not the battery's, and "set battery date" wrote over the UPS's date.
- Fixed: 32-bit timers that mean "no countdown" (-1) showed as 4294967295 on
  some Eaton, APC Smart-UPS and PowerWalker units.
- Values no working UPS reports (a 41216 V output, a 120% charge) are dropped
  rather than published.
- The Liebert GXT5 now reports runtime and on-mains/on-battery status. Its
  USB description switches usage tables mid-way, and the board read a status
  flag as the UPS's power rating. The board now follows the same rule as NUT.

## v0.14.1 -- 2026-09-30

- Fixed: on CyberPower UPSes, "turn the load off" with a delay waited 60 times
  too long. The page's default of 60 seconds meant an hour. These UPSes count
  the delay in minutes, and the board now converts it, rounding up so the
  outlets never go off sooner than asked. Readings are unaffected: in 65 real
  UPS descriptors, only the shutdown and startup delays use this conversion.

## v0.14 -- 2026-09-30

- Many more UPSes can now be read, including every Eaton tested, APC
  Smart-UPS models on USB ID 051d:0003 (Smart-UPS, SRT, Smart-UPS C),
  several CyberPower CP, OR and PR models, PowerWalker, IBM and Salicru.
  These UPSes mark their readings as fixed values in their USB description,
  and the board wrongly skipped them as padding, so it found no charge,
  runtime or status. Checked against 65 real UPS descriptors published on the
  NUT project's issue tracker: 55 now give charge, runtime and status, up
  from 29. Most of the rest are UPSes that do not speak USB HID at all.

## v0.13 -- 2026-09-30

- A UPS that keeps crashing the board can no longer knock it off the network.
  After three restarts in a row caused by the UPS, the board still identifies
  it but stops reading it; after five, it turns USB off. Either way it stays
  online, can be updated, and says so on its page, with a Try again button.
- When the board has no UPS readings (none attached, not read yet, or one it
  cannot read), NUT now answers "data stale", as a real NUT driver does,
  instead of reporting the UPS as OFF. Some NUT clients treat a UPS that stays
  OFF as critical and could shut a computer down.
- UPSes whose USB descriptor uses no report IDs can now be read. Before, the
  board rejected almost every reply from them and waited for a status forever.
- Values that sit past the 32nd byte of a report are now read, up to 63.
- A UPS the board has nothing to read from is no longer power-cycled over and
  over as if it had stopped answering.

## v0.12 -- 2026-09-30

- Fixed: some UPSes, including the APC Back-UPS BE600M1, made the board
  restart every couple of seconds and drop off the network as soon as they
  were plugged in. They answer a request with more data than was asked for,
  and the board now makes room for that instead of crashing. A board already
  stuck this way cannot update itself while the UPS is connected: unplug the
  UPS's USB cable, let the board update, then plug it back in.
- A UPS the board can't read now gets fixed faster. If a UPS is attached but
  gives no readings for five minutes, and the check-in is on, the board sends
  that UPS's USB report descriptor, make, model and USB ID to the project once.
  No readings, no serial number, nothing about your network.
- A "send my UPS info to the developer" button on the page sends the same
  thing on request, even with the check-in off.

## v0.11 -- 2026-09-29

- A real firmware target for a Rev A board with a VCC-first OLED module
  fitted (`reva-oled`), released alongside the plain build. It is a normal
  board choice with its own OTA identity, so a board flashed with it updates
  itself. A plain Rev A board is never offered it.

## v0.10 -- 2026-09-28

- A login is now required for every change, web and NUT. Set during Wi-Fi
  setup, or on the page's Login card if updating from an older version.
  Every write needs it: UPS commands, Wi-Fi, updates, reboot, and NUT's
  INSTCMD and SET VAR. Watching the UPS never does.
- A fixed IPv4 address, in Wi-Fi setup or a new Address card, for a NAS or
  Home Assistant pointed at the board's IP. Checked before it's saved: the
  router has to answer a ping from it.
- The page now warns when a UPS's "deep test" isn't a real one. Some models
  just rerun their short test; the page says so and shows the real duration
  for ones that do run a long one.
- Fixed a crash: a UPS control request that took over a second could free
  memory the USB stack still held, crashing the board later on an unrelated
  allocation.
- The page ran stale against new firmware if a tab was left open across an
  update. It now reloads itself when the firmware version changes, and is
  never cached.
- Failures on the page all read as one generic message. Now: "can't reach
  the board" versus a reply it can't parse versus a locked-out login, each
  said plainly.
- Fixed a false OFF over NUT: some UPSes read back all-zero status flags for
  a minute or two after a self-test, which used to map straight to OFF.
- A UPS's report cache has been seen to wedge shortly after a test starts.
  The board now power-cycles the USB link and re-reads right after a test
  command.
- Fixed a status-page header that shifted every second as its text changed
  length.
