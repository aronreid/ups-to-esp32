# Privacy

Your UPS's serial number, your Wi-Fi name and any address on your network never leave
the board, and the server never keeps your IP. The one exception is a board that offers
release candidates: see "Release candidates send diagnostic logs" below.

## The check-in

A short check-in (hardware ID, which board or build, firmware version, uptime, why the board last restarted,
where in the firmware it last crashed if it did,
and whether you chose to be offered release candidates)
is on by default so the board can be supported if something goes wrong. The page says
so, and one click turns it off. It never slows the board down: lowest priority, backs
off if the server is slow or unreachable, and the board behaves exactly the same either
way.

## If your UPS does not work

When a UPS is attached but the board cannot read it for five minutes, and the check-in
is on, the board sends that UPS's USB report descriptor once: the description of how
that model talks, plus its make, model and USB ID. No readings, no serial number,
nothing about your network. The web page also has a "send my UPS info to the developer"
button that sends the same thing whenever you press it, even with the check-in off.

For an APC Smart-UPS whose voltages the board reads over Modbus, the button also sends a
capture of that Modbus conversation, which includes the UPS's readings; with stats
ticked the board sends one such capture by itself once per boot.

Turn the check-in off and nothing is sent unprompted.

## Release candidates send diagnostic logs

A board set to offer release candidates (the checkbox on the page) sends its recent
diagnostic log to the server, so a bug in a test version can be found without you
copying logs by hand. The page says so next to the checkbox, and the Debug card shows
whether logs are being sent and when the last one went.

The log holds the board's own log lines: USB events, the UPS's make, model and serial
number, addresses on your own network, the name of the Wi-Fi network and what the
firmware was doing just before. Passwords, keys and other credentials, and every internet
address (yours and anyone else's), are removed on the board before anything is sent. A log is sent after the
board restarts from a crash, when the UPS fails to connect over USB, once a day, and
when the project asks for one, at most twelve a day. The server keeps logs for 30 days.

Untick "offer release candidates" and nothing is sent. Turning the check-in off stops
it as well. A board on full releases only never sends a log.

## Anonymous stats

Opt-in: tick the box and the board also shares the UPS's make, model and USB ID, its
readings, the battery date the UPS reports, the board's Wi-Fi signal and free memory, and
how many NUT clients are connected. Nothing that identifies you. Untick it and what was
kept is deleted.

## What the server holds

The database side is [`../telemetry/supabase.sql`](../telemetry/supabase.sql), so you
can see exactly what is stored, and that the board's key can do nothing with it except
check in and send a UPS's report descriptor.
