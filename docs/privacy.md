# Privacy

Your UPS's serial number, your Wi-Fi name and any address on your network never leave
the board, and the server never keeps your IP.

## The check-in

A short check-in (hardware ID, firmware version, uptime, why the board last restarted)
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

## Anonymous stats

Opt-in: tick the box and the board also shares the UPS's make and model, its readings,
Wi-Fi signal and free memory, nothing that identifies you. Untick it and what was kept
is deleted.

## What the server holds

The database side is [`../telemetry/supabase.sql`](../telemetry/supabase.sql), so you
can see exactly what is stored and that the board's key can do nothing but check in.
