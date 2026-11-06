/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * SNMP v1/v2c, the parts that do not need a network: BER in and out, the MIB
 * this board answers (MIB-2 system group and RFC 1628 UPS-MIB), and which
 * traps a change of UPS state calls for.
 *
 * Read-only, by design. A SET is answered with notWritable (noSuchName in
 * v1) and changes nothing: SNMPv2c sends its community string in clear, so
 * anything that could switch the UPS's load off stays behind the page's
 * login and NUT's.
 *
 * Kept free of ESP-IDF so tools/test/test_snmp.py can run it on the host,
 * against Net-SNMP's own snmpget/snmpwalk/snmptrapd.
 */
#ifndef UPSA_SNMP_CORE_H
#define UPSA_SNMP_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ups_data.h"

#define SNMP_COMMUNITY_MAX 32          /* longest community accepted, bytes */
#define SNMP_OID_MAX       32          /* sub-identifiers in a request OID  */
#define SNMP_REQ_VB_MAX    32          /* varbinds in one request           */
#define SNMP_RESP_VB_MAX   48          /* varbinds in one response (bulk)   */
#define SNMP_BULK_REPS_MAX 24          /* max-repetitions is clamped to this */

/* RFC 1628 upsWellKnownAlarms, 1.3.6.1.2.1.33.1.6.3.N. The alarm table's
 * index is the same number, so a given condition always has the same row. */
enum {
    SNMP_ALARM_BATTERY_BAD       = 1,
    SNMP_ALARM_ON_BATTERY        = 2,
    SNMP_ALARM_LOW_BATTERY       = 3,
    SNMP_ALARM_DEPLETED_BATTERY  = 4,
    SNMP_ALARM_TEMP_BAD          = 5,
    SNMP_ALARM_OUTPUT_OVERLOAD   = 8,
    SNMP_ALARM_ON_BYPASS         = 9,
    SNMP_ALARM_CHARGER_FAILED    = 13,
    SNMP_ALARM_UPS_OUTPUT_OFF    = 14,
    SNMP_ALARM_FAN_FAILURE       = 16,
    SNMP_ALARM_GENERAL_FAULT     = 18,
    SNMP_ALARM_TEST_FAILED       = 19,
    SNMP_ALARM_COMMS_LOST        = 20,
    SNMP_ALARM_AWAITING_POWER    = 21,
    SNMP_ALARM_SHUTDOWN_IMMINENT = 23,
    SNMP_ALARM_TEST_IN_PROGRESS  = 24,
    SNMP_ALARM_MAX               = 24,
};
#define SNMP_ALARM_BIT(id) (1u << ((id) - 1))

/* What the agent serves, built from the UPS snapshot by snmp_view_fill().
 * Integers are already in the MIB's units; SNMP_ABSENT where the UPS does
 * not report it, which the agent answers as noSuchInstance and leaves out of
 * a walk: never a made-up 0. */
#define SNMP_ABSENT INT32_MIN

typedef struct {
    /* system group */
    char     sys_descr[96];
    char     sys_name[64];
    char     sys_contact[64];
    char     sys_location[64];
    uint32_t uptime;               /* TimeTicks, hundredths since boot */

    /* upsIdent */
    char     mfr[32];
    char     model[64];
    char     ups_fw[64];
    char     agent_ver[64];

    /* live readings; SNMP_ABSENT while stale */
    int32_t  batt_status;          /* 1 unknown 2 normal 3 low 4 depleted */
    int32_t  secs_on_batt;
    int32_t  minutes_left;
    int32_t  charge;               /* % */
    int32_t  batt_dv;              /* 0.1 V */
    int32_t  batt_temp;            /* degrees C */
    uint32_t line_bads;            /* Counter32: transfers to battery */
    int32_t  in_dhz;               /* 0.1 Hz */
    int32_t  in_v;                 /* V */
    int32_t  in_da;                /* 0.1 A */
    int32_t  out_source;           /* 1 other 2 none 3 normal 4 bypass 5 battery 6 booster 7 reducer */
    int32_t  out_dhz;
    int32_t  out_v;
    int32_t  out_da;               /* 0.1 A */
    int32_t  out_w;                /* W, measured only */
    int32_t  out_load;             /* % */
    int32_t  test_summary;         /* upsTestResultsSummary 1..6 */
    const char *test_detail;       /* static string */
    int32_t  test_start;           /* upsTestStartTime, sysUpTime; SNMP_ABSENT if unknown */
    int32_t  test_elapsed;         /* upsTestElapsedTime, 0.01 s; SNMP_ABSENT if unknown */
    int32_t  cfg_in_v, cfg_out_v, cfg_out_w;
    int32_t  cfg_in_dhz, cfg_out_dhz;  /* nominal frequencies, 0.1 Hz */
    int32_t  cfg_out_va;           /* VA */
    int32_t  cfg_low_batt_min;     /* upsConfigLowBattTime, minutes */
    int32_t  cfg_xfer_lo, cfg_xfer_hi; /* transfer points, V */
    int32_t  audible;              /* 1 disabled 2 enabled 3 muted */

    uint32_t alarms;               /* SNMP_ALARM_BIT()s present */
    uint32_t alarm_time[SNMP_ALARM_MAX + 1];  /* sysUpTime when raised; 0 if at start */
} snmp_view_t;

/* What only the caller knows, alongside ups_data_t. Floats are NAN-free:
 * UPS_VALUE_ABSENT where absent, as in ups_data_t. */
typedef struct {
    const char *nut_status;        /* NUT's ups.status, e.g. "OL CHRG"; "" none */
    bool  stale;                   /* NUT would answer DATA-STALE */
    bool  comms_grace;             /* too soon after boot to call a missing UPS lost */
    float batt_temp, out_freq, out_current, in_nominal, out_nominal;
    const char *ups_fw;
    /* NUT's status tokens (ups_nutstatus.h NST() bits), for the faults that
     * reach only ups.alarm: overheat, charger, fan, internal fault... */
    bool     nst_valid;
    uint32_t nst_bits;
    float in_current, in_freq_nominal, out_freq_nominal, va_nominal;
    float low_batt_s;              /* battery.runtime.low, seconds */
    float xfer_low, xfer_high;     /* input.transfer.low / .high, V */
} snmp_extra_t;

/* State carried from one snapshot to the next: when each alarm began, when
 * the UPS went to battery, the line-bad count, and the last good alarm set,
 * which is held (not cleared) while the UPS cannot be read. */
typedef struct {
    bool     started;
    uint32_t alarms;
    uint32_t alarm_time[SNMP_ALARM_MAX + 1];
    int64_t  ob_since_us;
    uint32_t line_bads;
    int32_t  test_summary;         /* last seen; 0 before the first */
    int32_t  test_start;           /* sysUpTime the running/last test began; SNMP_ABSENT unknown */
    int32_t  test_end;             /* sysUpTime it finished; SNMP_ABSENT while running/unknown */
} snmp_track_t;

/* Identity strings (sys_*, agent_ver) are the caller's: this fills the rest. */
void snmp_view_fill(snmp_track_t *t, snmp_view_t *v, const ups_data_t *d,
                    const snmp_extra_t *x, int64_t now_us);

/* --- the agent ---------------------------------------------------------- */

typedef struct {
    uint32_t in_pkts, bad_community, bad_version, parse_errors, answered;
} snmp_stats_t;

/* Answer one request datagram. Returns the response length in `out`, or 0
 * to send nothing: malformed, wrong community, wrong version, or not a
 * request. `community` must be non-empty; compared in constant time. */
size_t snmp_handle(const uint8_t *req, size_t len, uint8_t *out, size_t cap,
                   const char *community, const snmp_view_t *v, snmp_stats_t *st);

/* True if the MIB table is in strict OID order (a walk depends on it). */
bool snmp_mib_sorted(void);

/* --- traps -------------------------------------------------------------- */

typedef enum {
    SNMP_TRAP_COLD_START,          /* SNMPv2-MIB coldStart, once per boot */
    SNMP_TRAP_WARM_START,          /* the page's "send a test trap"       */
    SNMP_TRAP_ON_BATTERY,          /* upsTrapOnBattery, resent each minute */
    SNMP_TRAP_TEST_COMPLETED,      /* upsTrapTestCompleted                 */
    SNMP_TRAP_ALARM_ADDED,         /* upsTrapAlarmEntryAdded               */
    SNMP_TRAP_ALARM_REMOVED,       /* upsTrapAlarmEntryRemoved             */
} snmp_trap_t;

typedef struct {
    snmp_trap_t kind;
    int         alarm;             /* for ALARM_ADDED/REMOVED */
} snmp_event_t;

/* RFC 1628: no notification to one receiver more often than every five
 * seconds; upsTrapOnBattery repeats every minute while on battery. */
#define SNMP_TRAP_SPACING_US   (5LL * 1000 * 1000)
#define SNMP_ONBATT_REPEAT_US  (60LL * 1000 * 1000)

/* Which traps the receiver has been told about. The difference between this
 * and the view is what is still owed, so a change that is held back by the
 * five-second spacing is sent once the spacing allows, and a flap that
 * settles back before then sends nothing. */
typedef struct {
    bool     cold_pending;
    uint32_t told;                 /* alarms the receiver knows are present */
    int64_t  last_us[SNMP_ALARM_MAX + 1];
    int64_t  onbatt_us;
    int32_t  test_summary;         /* last seen; 0 before the first view */
} snmp_ev_t;

/* A fresh state: nothing told, coldStart owed if `cold`. */
void snmp_ev_reset(snmp_ev_t *ev, bool cold);

/* Traps owed now, up to `max`, in the order to send them. Marks them told. */
int  snmp_events(snmp_ev_t *ev, const snmp_view_t *v, int64_t now_us,
                 snmp_event_t *out, int max);

/* An SNMPv2-Trap PDU in a v2c message. Returns its length, 0 if it did not
 * fit. */
size_t snmp_build_trap(uint8_t *out, size_t cap, const char *community,
                       uint32_t request_id, const snmp_view_t *v, const snmp_event_t *e);

/* --- settings ----------------------------------------------------------- */

typedef struct {
    bool     agent;                         /* answer GET/GETNEXT/GETBULK on UDP 161 */
    bool     traps;                         /* send traps to trap_host */
    char     community[SNMP_COMMUNITY_MAX + 1];
    char     trap_host[16];                 /* IPv4, dotted quad */
    uint16_t trap_port;                     /* 162 unless changed */
    char     location[64];                  /* sysLocation */
    char     contact[64];                   /* sysContact */
} snmp_cfg_t;

#define SNMP_TRAP_PORT_DEFAULT 162

/* A dotted-quad IPv4 address a trap can be sent to: not 0.0.0.0, not
 * 255.255.255.255, not 127.x, not multicast. *out in network byte order. */
bool snmp_ipv4_ok(const char *s, uint32_t *out);

/* Whether `c` can be saved; if not, a sentence for the page in `why`. */
bool snmp_cfg_check(const snmp_cfg_t *c, char *why, size_t why_len);

#endif /* UPSA_SNMP_CORE_H */
