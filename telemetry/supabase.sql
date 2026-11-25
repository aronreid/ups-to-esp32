-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- The fleet check-in, on Supabase: everything the server holds about a board.
-- Run once in the project's SQL editor; safe to run again.
--
-- Boards hold only the project's PUBLIC key (it is in the open-source
-- firmware), so nothing here trusts the caller. Row-level security is on for
-- every table and there are NO policies: the public key can neither read nor
-- write a table directly. Its only doors are two functions:
--   heartbeat()          the check-in. Validates its input, touches only the
--                        calling board's own row, answers with the update
--                        channel and whether the server would like the UPS's
--                        report descriptor.
--   upload_descriptor()  the UPS's USB report descriptor (how that model
--                        talks), sent when a UPS cannot be read or when the
--                        owner presses "send my UPS info". Accepted only from
--                        a board that has checked in, only as hex that matches
--                        its own hash.
-- Someone can still post check-ins for a MAC they make up; that pollutes the
-- stats, and reaches nothing else.
--
-- No IP address, Wi-Fi name or UPS serial is stored. The MAC is kept because
-- it is how the project's own board records know the same board.

create table if not exists public.devices (
    mac              text primary key check (mac ~ '^([0-9a-f]{2}:){5}[0-9a-f]{2}$'),
    first_seen       timestamptz not null default now(),
    last_seen        timestamptz not null default now(),
    firmware_version text,
    board            text,                -- image name, e.g. ups-adaptor-reva
    uptime_s         bigint,
    reset_reason     text,
    heartbeats       bigint not null default 0,
    -- Set by people, never by a board: 'production' boards are offered full
    -- releases; 'dev' and 'debug' boards are the project's own test boards.
    channel          text not null default 'production'
                     check (channel in ('production', 'debug', 'dev')),
    label            text not null default '',
    stats            jsonb,               -- only when the owner ticked "stats"
    stats_at         timestamptz,
    last_crash       text,                -- where in the firmware it last crashed
    last_crash_at    timestamptz,
    rc_opt           boolean              -- owner chose "offer release candidates"
);

create table if not exists public.heartbeats (
    id               bigserial primary key,
    mac              text not null references public.devices(mac) on delete cascade,
    at               timestamptz not null default now(),
    firmware_version text,
    uptime_s         bigint,
    stats            jsonb,
    crash            text
);
create index if not exists heartbeats_mac_at on public.heartbeats (mac, at desc);

-- Update channels. Set by people; a board only reads its own channel's row.
create table if not exists public.channels (
    name         text primary key check (name in ('production', 'debug', 'dev')),
    target       text check (target is null or target ~ '^v[0-9][0-9A-Za-z.-]{0,30}$'),
    auto_install boolean not null default false,
    note         text not null default ''
);

-- One row per UPS model's report descriptor (no readings, no serial).
create table if not exists public.descriptors (
    hash       text primary key check (hash ~ '^[0-9a-f]{16}$'),
    ups_id     text,                      -- USB vendor:product, e.g. 0764:0501
    model      text,
    length     integer,
    hex        text,                      -- the descriptor, once sent
    first_mac  text,
    first_seen timestamptz not null default now(),
    in_tests   boolean not null default false,
    note       text not null default ''
);

alter table public.devices     enable row level security;
alter table public.heartbeats  enable row level security;
alter table public.channels    enable row level security;
alter table public.descriptors enable row level security;
revoke all on public.devices, public.heartbeats, public.channels, public.descriptors
    from anon, authenticated;

-- Older versions of the check-in took fewer arguments. Only the current one
-- may exist: PostgREST refuses a call that matches two of them.
drop function if exists public.heartbeat(text, text, text, bigint, text, jsonb);
drop function if exists public.heartbeat(text, text, text, bigint, text, jsonb, text);

create or replace function public.heartbeat(
    p_mac          text,
    p_version      text,
    p_board        text,
    p_uptime_s     bigint,
    p_reset_reason text    default null,
    p_stats        jsonb   default null,
    p_crash        text    default null,
    p_rc           boolean default null
) returns jsonb
language plpgsql
security definer
set search_path = public
as $$
declare
    m         text := lower(coalesce(p_mac, ''));
    prev      timestamptz;
    ch        text;
    c         channels%rowtype;
    dh        text := p_stats->>'desc_hash';
    want      boolean := false;
    k         text;
    -- A release tag (v0.16, v0.16.1-rc2) or a git-describe build string:
    -- letters, digits, +, _, . and - only.
    version_re constant text := '^[A-Za-z0-9+_.-]{1,40}$';
    device_cap constant int  := 20000;
begin
    if m !~ '^([0-9a-f]{2}:){5}[0-9a-f]{2}$' then
        raise exception 'bad mac' using errcode = '22023';
    end if;
    if length(coalesce(p_version, '')) > 40 or length(coalesce(p_board, '')) > 40
       or length(coalesce(p_reset_reason, '')) > 24 or length(coalesce(p_crash, '')) > 240 then
        raise exception 'field too long' using errcode = '22023';
    end if;
    if p_version is not null and p_version <> '' and p_version !~ version_re then
        raise exception 'bad version' using errcode = '22023';
    end if;
    -- The stats a board may send (only when its owner opted in), each with
    -- its type. Anything else in the object is ignored by the project.
    if p_stats is not null then
        if jsonb_typeof(p_stats) <> 'object' or length(p_stats::text) > 2048 then
            raise exception 'bad stats' using errcode = '22023';
        end if;
        foreach k in array array['ups_status', 'runtime_age_s', 'load_pct',
                                 'battery_charge', 'desc_len', 'input_v',
                                 'realpower_nom_w', 'rssi', 'nut_clients', 'heap_low'] loop
            if p_stats ? k and jsonb_typeof(p_stats -> k) <> 'number' then
                raise exception 'bad stats.%', k using errcode = '22023';
            end if;
        end loop;
        foreach k in array array['ups_mfr', 'ups_model', 'ups_id', 'desc_hash', 'battery_date'] loop
            if p_stats ? k and jsonb_typeof(p_stats -> k) <> 'string' then
                raise exception 'bad stats.%', k using errcode = '22023';
            end if;
        end loop;
    end if;
    if not exists (select 1 from devices where mac = m)
       and (select count(*) from devices) >= device_cap then
        raise exception 'fleet directory is full' using errcode = '22023';
    end if;

    select last_seen into prev from devices where mac = m;

    insert into devices as d (mac, firmware_version, board, uptime_s, reset_reason,
                              heartbeats, stats, stats_at, last_crash, last_crash_at, rc_opt)
    values (m, p_version, p_board, p_uptime_s, p_reset_reason, 1,
            p_stats, case when p_stats is null then null else now() end,
            p_crash, case when p_crash is null then null else now() end, p_rc)
    on conflict (mac) do update set
        last_seen        = now(),
        firmware_version = excluded.firmware_version,
        board            = excluded.board,
        uptime_s         = excluded.uptime_s,
        reset_reason     = excluded.reset_reason,
        heartbeats       = d.heartbeats + 1,
        -- opting out clears what was sent before, not just what comes next
        stats            = excluded.stats,
        stats_at         = excluded.stats_at,
        rc_opt           = excluded.rc_opt,     -- null: the board did not say
        last_crash       = coalesce(excluded.last_crash, d.last_crash),
        last_crash_at    = case when excluded.last_crash is distinct from d.last_crash
                                     and excluded.last_crash is not null
                                then now() else d.last_crash_at end
    returning channel into ch;

    -- History, at most one row a minute a board, kept 90 days.
    if prev is null or now() - prev > interval '1 minute' then
        insert into heartbeats (mac, firmware_version, uptime_s, stats, crash)
        values (m, p_version, p_uptime_s, p_stats, p_crash);
    end if;
    if random() < 0.01 then
        delete from heartbeats where at < now() - interval '90 days';
    end if;

    -- Every UPS model heard, and whether its descriptor is still wanted.
    if dh ~ '^[0-9a-f]{16}$' then
        insert into descriptors (hash, ups_id, model, first_mac)
        values (dh, left(p_stats->>'ups_id', 16), left(p_stats->>'ups_model', 64), m)
        on conflict (hash) do nothing;
        select hex is null into want from descriptors where hash = dh;
    end if;

    select * into c from channels where name = ch;
    return jsonb_build_object('channel', ch, 'interval_s', 21600,
                              'target', c.target,
                              'auto_install', coalesce(c.auto_install, false) and ch <> 'production',
                              'want_descriptor', coalesce(want, false));
end;
$$;

create or replace function public.upload_descriptor(
    p_mac    text,
    p_hash   text,
    p_ups_id text,
    p_model  text,
    p_hex    text
) returns jsonb
language plpgsql
security definer
set search_path = public
as $$
declare
    m text := lower(coalesce(p_mac, ''));
    h text := lower(coalesce(p_hash, ''));
    x text := lower(coalesce(p_hex, ''));
begin
    -- Only from a board that has checked in, only hex, only what it claims.
    if not exists (select 1 from devices where mac = m) then
        raise exception 'unknown board' using errcode = '22023';
    end if;
    if h !~ '^[0-9a-f]{16}$' or x !~ '^([0-9a-f]{2})+$' or length(x) > 8192 then
        raise exception 'bad descriptor' using errcode = '22023';
    end if;
    if left(encode(sha256(decode(x, 'hex')), 'hex'), 16) <> h then
        raise exception 'descriptor does not match its hash' using errcode = '22023';
    end if;
    insert into descriptors as d (hash, ups_id, model, length, hex, first_mac)
    values (h, left(p_ups_id, 16), left(p_model, 64), length(x) / 2, x, m)
    on conflict (hash) do update set
        hex    = coalesce(d.hex, excluded.hex),
        length = coalesce(d.length, excluded.length);
    return jsonb_build_object('ok', true);
end;
$$;

revoke all on function public.heartbeat(text, text, text, bigint, text, jsonb, text, boolean) from public;
grant execute on function public.heartbeat(text, text, text, bigint, text, jsonb, text, boolean) to anon;
revoke all on function public.upload_descriptor(text, text, text, text, text) from public;
grant execute on function public.upload_descriptor(text, text, text, text, text) to anon;
