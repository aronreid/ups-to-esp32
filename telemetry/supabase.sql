-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- The fleet heartbeat, on Supabase. Run once in the project's SQL editor.
--
-- Boards hold only the project's PUBLIC key (it is in the open-source
-- firmware), so nothing here trusts the caller. Row-level security is on for
-- every table and there are NO policies: the public key can neither read nor
-- write a table directly. Its one door is heartbeat(), which validates its
-- input, touches only the calling board's own row, and answers with the
-- update channel. Someone can still post heartbeats for a MAC they make up;
-- that pollutes the stats, and reaches nothing else.
--
-- No IP address, Wi-Fi name or UPS serial is stored. The MAC is kept because
-- it is how the flash station's board database knows the same board.

create table if not exists public.devices (
    mac              text primary key check (mac ~ '^([0-9a-f]{2}:){5}[0-9a-f]{2}$'),
    first_seen       timestamptz not null default now(),
    last_seen        timestamptz not null default now(),
    firmware_version text,
    board            text,                -- image name, e.g. ups-adaptor-reva
    uptime_s         bigint,
    reset_reason     text,
    heartbeats       bigint not null default 0,
    -- set by people, in the dashboard: 'production' boards get full releases,
    -- 'dev' boards also get release candidates.
    channel          text not null default 'production'
                     check (channel in ('production', 'dev')),
    label            text not null default '',
    stats            jsonb,               -- only when the owner opted in
    stats_at         timestamptz
);

create table if not exists public.heartbeats (
    id               bigserial primary key,
    mac              text not null references public.devices(mac) on delete cascade,
    at               timestamptz not null default now(),
    firmware_version text,
    uptime_s         bigint,
    stats            jsonb
);
create index if not exists heartbeats_mac_at on public.heartbeats (mac, at desc);

alter table public.devices    enable row level security;
alter table public.heartbeats enable row level security;
revoke all on public.devices, public.heartbeats from anon, authenticated;

create or replace function public.heartbeat(
    p_mac          text,
    p_version      text,
    p_board        text,
    p_uptime_s     bigint,
    p_reset_reason text  default null,
    p_stats        jsonb default null
) returns jsonb
language plpgsql
security definer
set search_path = public
as $$
declare
    m    text := lower(coalesce(p_mac, ''));
    prev timestamptz;
    ch   text;
begin
    if m !~ '^([0-9a-f]{2}:){5}[0-9a-f]{2}$' then
        raise exception 'bad mac' using errcode = '22023';
    end if;
    if length(coalesce(p_version, '')) > 40 or length(coalesce(p_board, '')) > 40
       or length(coalesce(p_reset_reason, '')) > 24 then
        raise exception 'field too long' using errcode = '22023';
    end if;
    if p_stats is not null and (jsonb_typeof(p_stats) <> 'object'
                                or length(p_stats::text) > 2048) then
        raise exception 'bad stats' using errcode = '22023';
    end if;

    select last_seen into prev from devices where mac = m;

    insert into devices as d (mac, firmware_version, board, uptime_s, reset_reason,
                              heartbeats, stats, stats_at)
    values (m, p_version, p_board, p_uptime_s, p_reset_reason, 1,
            p_stats, case when p_stats is null then null else now() end)
    on conflict (mac) do update set
        last_seen        = now(),
        firmware_version = excluded.firmware_version,
        board            = excluded.board,
        uptime_s         = excluded.uptime_s,
        reset_reason     = excluded.reset_reason,
        heartbeats       = d.heartbeats + 1,
        -- opting out clears what was sent before, not just what comes next
        stats            = excluded.stats,
        stats_at         = excluded.stats_at
    returning channel into ch;

    -- History, at most one row a minute a board, kept 90 days.
    if prev is null or now() - prev > interval '1 minute' then
        insert into heartbeats (mac, firmware_version, uptime_s, stats)
        values (m, p_version, p_uptime_s, p_stats);
    end if;
    if random() < 0.01 then
        delete from heartbeats where at < now() - interval '90 days';
    end if;

    return jsonb_build_object('channel', ch, 'interval_s', 21600);
end;
$$;

revoke all on function public.heartbeat(text, text, text, bigint, text, jsonb) from public;
grant execute on function public.heartbeat(text, text, text, bigint, text, jsonb) to anon;
