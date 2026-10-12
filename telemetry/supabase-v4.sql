-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- v4 of the fleet heartbeat, on top of supabase-v3.sql. Safe to run more
-- than once. Firmware that sends Modbus captures needs it; without it their
-- upload_modbus() calls fail (HTTP 404) and nothing else changes.
--
-- APC Smart-UPS units on 051d:0003 keep their voltages, load and power out of
-- HID, in APC's Modbus register map, which the board reads over the same USB
-- cable (firmware/components/ups_hid/include/apc_modbus.h). No such unit has
-- been on the bench, so a capture from the field is how its replies get
-- checked: the raw registers, the last exchanges with their reply frames, and
-- counts of answers and timeouts, exactly as /api/modbus serves them.
--
-- The board sends one when the owner presses "send my UPS info", and once per
-- boot by itself when stats are ticked (or on a test board): the capture holds
-- readings, which the stats box is the consent for. Only from a board that has
-- checked in, only a JSON object, at most 16 KB, and the newest 20 per board.

create table if not exists public.modbus_captures (
    id         bigserial primary key,
    mac        text not null,
    desc_hash  text,                 -- descriptors.hash of the UPS it came from
    ups_id     text,                 -- VID:PID
    model      text,
    firmware   text,
    reason     text,                 -- 'owner' or 'auto'
    captured   timestamptz not null default now(),
    capture    jsonb not null
);
create index if not exists modbus_captures_mac on public.modbus_captures (mac, captured desc);
alter table public.modbus_captures enable row level security;
revoke all on public.modbus_captures from anon, authenticated;

create or replace function public.upload_modbus(
    p_mac      text,
    p_hash     text,
    p_ups_id   text,
    p_model    text,
    p_version  text,
    p_reason   text,
    p_capture  jsonb
) returns jsonb
language plpgsql
security definer
set search_path = public
as $$
declare
    m text := lower(coalesce(p_mac, ''));
begin
    if not exists (select 1 from devices where mac = m) then
        raise exception 'unknown board' using errcode = '22023';
    end if;
    if p_capture is null or jsonb_typeof(p_capture) <> 'object'
       or length(p_capture::text) > 16384 then
        raise exception 'bad capture' using errcode = '22023';
    end if;
    if coalesce(p_hash, '') !~ '^([0-9a-f]{16})?$' then
        raise exception 'bad hash' using errcode = '22023';
    end if;
    insert into modbus_captures (mac, desc_hash, ups_id, model, firmware, reason, capture)
    values (m, nullif(p_hash, ''), left(p_ups_id, 16), left(p_model, 64),
            left(p_version, 40), left(p_reason, 8), p_capture);
    delete from modbus_captures
     where mac = m
       and id not in (select id from modbus_captures where mac = m
                      order by captured desc limit 20);
    return jsonb_build_object('ok', true);
end;
$$;

revoke all on function public.upload_modbus(text, text, text, text, text, text, jsonb) from public;
grant execute on function public.upload_modbus(text, text, text, text, text, text, jsonb) to anon;
