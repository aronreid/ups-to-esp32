-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- v2 of the fleet heartbeat, applied on top of supabase.sql. Safe to run more
-- than once. Firmware v0.09-rc2 and later need it; earlier heartbeats still
-- work against it (their calls have no p_crash, which defaults to null).
--
-- Three release tiers, per board (devices.channel):
--   production  full releases, offered to the owner
--   debug       the one release named in channels.target, installed by itself
--   dev         channels.target if named, else the newest build of any kind,
--               installed by itself
-- channels holds the per-tier settings; editing a row there moves every board
-- on that tier at its next heartbeat.

create table if not exists public.channels (
    name         text primary key check (name in ('production', 'debug', 'dev')),
    target       text check (target is null or target ~ '^v[0-9][0-9A-Za-z.-]{0,30}$'),
    auto_install boolean not null default false,
    note         text not null default ''
);
insert into public.channels (name, target, auto_install, note) values
    ('production', null, false, 'full releases, offered to the owner'),
    ('debug',      null, true,  'set target to a release candidate that has proved stable'),
    ('dev',        null, true,  'newest build of any kind unless target is set')
on conflict (name) do nothing;
alter table public.channels enable row level security;
revoke all on public.channels from anon, authenticated;

-- devices.channel gains 'debug'. The original check was declared inline, so
-- its name is Postgres's; drop whichever check constrains channel.
do $$
declare c text;
begin
    for c in select conname from pg_constraint
             where conrelid = 'public.devices'::regclass and contype = 'c'
               and pg_get_constraintdef(oid) like '%channel%'
    loop
        execute format('alter table public.devices drop constraint %I', c);
    end loop;
end $$;
alter table public.devices add constraint devices_channel_check
    check (channel in ('production', 'debug', 'dev'));

-- Where a board last crashed: the panic's reason and a backtrace, to decode
-- against the release's ELF. No personal data.
alter table public.devices    add column if not exists last_crash    text;
alter table public.devices    add column if not exists last_crash_at timestamptz;
alter table public.heartbeats add column if not exists crash         text;

-- The new signature replaces the old one outright: two overloads would leave
-- PostgREST unable to choose.
drop function if exists public.heartbeat(text, text, text, bigint, text, jsonb);

create or replace function public.heartbeat(
    p_mac          text,
    p_version      text,
    p_board        text,
    p_uptime_s     bigint,
    p_reset_reason text  default null,
    p_stats        jsonb default null,
    p_crash        text  default null
) returns jsonb
language plpgsql
security definer
set search_path = public
as $$
declare
    m    text := lower(coalesce(p_mac, ''));
    prev timestamptz;
    ch   text;
    c    channels%rowtype;
begin
    if m !~ '^([0-9a-f]{2}:){5}[0-9a-f]{2}$' then
        raise exception 'bad mac' using errcode = '22023';
    end if;
    if length(coalesce(p_version, '')) > 40 or length(coalesce(p_board, '')) > 40
       or length(coalesce(p_reset_reason, '')) > 24 or length(coalesce(p_crash, '')) > 240 then
        raise exception 'field too long' using errcode = '22023';
    end if;
    if p_stats is not null and (jsonb_typeof(p_stats) <> 'object'
                                or length(p_stats::text) > 2048) then
        raise exception 'bad stats' using errcode = '22023';
    end if;

    select last_seen into prev from devices where mac = m;

    insert into devices as d (mac, firmware_version, board, uptime_s, reset_reason,
                              heartbeats, stats, stats_at, last_crash, last_crash_at)
    values (m, p_version, p_board, p_uptime_s, p_reset_reason, 1,
            p_stats, case when p_stats is null then null else now() end,
            p_crash, case when p_crash is null then null else now() end)
    on conflict (mac) do update set
        last_seen        = now(),
        firmware_version = excluded.firmware_version,
        board            = excluded.board,
        uptime_s         = excluded.uptime_s,
        reset_reason     = excluded.reset_reason,
        heartbeats       = d.heartbeats + 1,
        stats            = excluded.stats,
        stats_at         = excluded.stats_at,
        -- A board repeats its last crash on every heartbeat until it has a new
        -- one: the time is when it was first heard, not each repeat.
        last_crash       = coalesce(excluded.last_crash, d.last_crash),
        last_crash_at    = case when excluded.last_crash is distinct from d.last_crash
                                     and excluded.last_crash is not null
                                then now() else d.last_crash_at end
    returning channel into ch;

    if prev is null or now() - prev > interval '1 minute' then
        insert into heartbeats (mac, firmware_version, uptime_s, stats, crash)
        values (m, p_version, p_uptime_s, p_stats, p_crash);
    end if;
    if random() < 0.01 then
        delete from heartbeats where at < now() - interval '90 days';
    end if;

    select * into c from channels where name = ch;
    return jsonb_build_object('channel', ch, 'interval_s', 21600,
                              'target', c.target,
                              'auto_install', coalesce(c.auto_install, false) and ch <> 'production');
end;
$$;

revoke all on function public.heartbeat(text, text, text, bigint, text, jsonb, text) from public;
grant execute on function public.heartbeat(text, text, text, bigint, text, jsonb, text) to anon;
