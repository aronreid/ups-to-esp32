-- SPDX-License-Identifier: GPL-3.0-or-later
--
-- v3 of the fleet heartbeat, on top of supabase-v2.sql. Safe to run more
-- than once. Every UPS model in the field, by the fingerprint of its HID
-- report descriptor (first 8 bytes of its SHA-256, as hex).
--
-- Vendors put runtime and charge on different usages with different scaling,
-- which is why NUT has per-vendor subdrivers; two UPSes with the same
-- descriptor are read the same way, and a new fingerprint is a model the
-- parser has not been tested on. heartbeat() records each fingerprint it
-- hears; when it has no copy of that descriptor it asks the board for one
-- (want_descriptor), and upload_descriptor() takes it. The board decides when
-- to send: with stats ticked (or on a test board) when asked, when its UPS
-- enumerates but cannot be read for five minutes, or when the owner presses
-- "send my UPS info". The server only requires that the board has checked in.

create table if not exists public.descriptors (
    hash       text primary key check (hash ~ '^[0-9a-f]{16}$'),
    ups_id     text,                 -- VID:PID
    model      text,
    length     int,
    hex        text,                 -- the descriptor itself, once uploaded
    first_mac  text,
    first_seen timestamptz not null default now(),
    in_tests   boolean not null default false,  -- a capture of it is in the repo's regression tests
    note       text not null default ''
);
alter table public.descriptors enable row level security;
revoke all on public.descriptors from anon, authenticated;

-- The captures already in the repository, which the parser is tested against.
insert into public.descriptors (hash, ups_id, model, length, hex, in_tests, note) values
    ('af9a587a6661c286', '051d:0002', 'Back-UPS RS 1000G', 1133, '05840904a1010924a100850109fe790275089501150026ff00b122850209ff7903b1228503058509897904b1228504098f7901b1228505098bb1228506094481a20944b1a2094581a20945b1a20686ff096081a20960b1a2850705850985751027ffff0000b1a28508058409406721d1f0005505b12285090930b1a2850a09fd750826ff00650055007901b122850b0585092cb122850c0966256481a20966b1a20968751027ffff000066011081a20968b1a2850d0983750825646500b122850e0967b122850f098cb1228510098eb122851109291501b1a28512098d1500b122851309d0250181a209d0b1a28514094281a20942b1a20584096981a20969b1a285150957751016ffff26ff7f660110b1a20902a102851605856500750115002501094481a20944b1a2094581a20945b1a209d081a209d0b1a209d181a209d1b1a2094281a20942b1a20584096981a20969b1a20585094381a20943b1a20584097381a20973b1a20585094b81a2094bb1a20584096581a20965b1a2058509db81a209dbb1a295158101b101c08517092a9501751027ffff0000660110b1a285180584095a7508150125036500b1a2c00912a100851c0686ff09167518150027ffffff00b2a201852005850985751027ffff0000b1a28521058409587508250681a20958b1a28522058509662564b1a285230968751027ffff0000660110b1a28524092ab1a28525058409406721d1f0005505b12285260930b1a285270686ff0924751016e90026fe0065005500b1a2852809187520170100008027ffffff7fb2a201c00584091aa1008530058409407508150026ff006721d1f0005507b122853109307510b1a285320953164e00265800b1a285330954168800268e00b1a285340686ff0924167500268b0065005500b1a285357508096115002502b1a285360952250db1a2c00905a1008540097c2501b1a28541097d751016ffff26ff7f660110b1a2854205840957b1a2c00916a100855009357508150025646500b1a285510686ff092416be0026fe00b1a28552058409447510150027ffff00006621d15507b1a2c00686ff0901a1008560092365005500b1a2856109267508161000268d00b1a2856209257520170100008027ffffff7fb2a201c0857f058409fe790575089501150026ff00b122857e0686ff09427907b122857d058409ff7903b122857c09fd7901b122857b05850985751027ffff0000b1a205840902a102857a0585094475012501b1a20945b1a209d0b1a209d1b1a20942b1a205840969b1a205850943b1a205840973b1a20585094bb1a205840965b1a2058509dbb1a29515b101c085790686ff097275089501b1a285780584095a15012503b1a285750686ff09297510150027ffff0000b1a285740686ff092a7520170100008027ffffff7fb1a20686ff0990a100858c099175089501150026ff00b1a2858d0992150a2550b1a2858e099315002564b1a2858f099415002501b1a28590099515002502b1a2859109967510150027100e0000b1a285920997b1a2c0a1000600ff85800955150026ff0075089501b182c0c0', true, 'captures/051d-0002-20260917-125739'),
    ('67d87aad3ef7c1b5', '0764:0501', 'EC850LCD', 496, '05840904a1010924a100850109fe75089501150026ff00b122850209ffb12285290601ff099eb122099e8123750826ff00850305850989b1228504098fb1228505098bb1228506092cb12285077508950625640983098d098e098c09290967b1228508750895016500096681230966b1a20968751027ffff000066011081230968b1a2092a2658028123092ab1a28509750826ff00058409406721d1f0005506b122850a0930b1a2650055000902a102850b750195062501058509d009440945094209460943812309d009440945094209460943b1a2750295018101b101c0850c0584095a750815012503b1a2095a8123850d09fd150026ff00b122c00584091aa100850e058409407510150026e6006721d1f0005507b122850f0930b1a20930812385100953165b00266400b122095381230954168700269000b12209548123c0091ca100851209301500262c01b1a265005500750885130935b1a2750125010965b1a27507b1017508851409582506b1a20958812385150957751015ff26ff7f35c447c4ff1d00660110b1a285160956b1a23500450085187510094426c2016621d15507b1a26500550015000601ff852a750125010944b122094481237507b1018101c00601ff09baa102650055001500852b751027ffff000009bbb122750826ff0009bcb12209bdb2a201c0c0', true, 'captures/0764-0501-ec850lcd-20260921')
on conflict (hash) do update set in_tests = true, hex = coalesce(descriptors.hex, excluded.hex);

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
    dh   text := p_stats->>'desc_hash';
    want boolean := false;
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

revoke all on function public.heartbeat(text, text, text, bigint, text, jsonb, text) from public;
grant execute on function public.heartbeat(text, text, text, bigint, text, jsonb, text) to anon;
revoke all on function public.upload_descriptor(text, text, text, text, text) from public;
grant execute on function public.upload_descriptor(text, text, text, text, text) to anon;
