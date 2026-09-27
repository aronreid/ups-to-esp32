#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The optional login: off by default, and when on, every change needs it.

Two halves.

1. firmware/components/auth/auth_core.c compiled on the host and run: with no
   login set everything is allowed, exactly as before; with one set, wrong
   credentials are refused, five in a row lock checks out for a minute, tokens
   expire and are revoked when the login changes, NUT gets upsd's own refusals
   in upsd's order, and a request a browser sends from another page is refused
   whether or not a login is set.

2. The wiring, read from the source: every POST route in webui.c except login
   and logout starts with may_write(); both NUT writes (INSTCMD, SET VAR) go
   through nut_may_write(); a NUT PASSWORD is never logged; and the BOOT reset
   erases the namespace the login lives in. A route added without the check is
   a way round the whole feature, which is why this is a test and not a
   comment.
"""
import os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FW = os.path.join(ROOT, "firmware")
AUTH = os.path.join(FW, "components", "auth")

TESTS = r'''
#include <stdio.h>
#include <string.h>
#include "auth_core.h"

static int fails = 0;
#define CHECK(name, cond) do { if (cond) printf("  ok   %s\n", name); \
    else { printf("  FAIL %s\n", name); fails++; } } while (0)

/* Not SHA-256: the core's logic is under test, not the hash, which is
 * mbedtls's on the board. Salt and password both change the output. */
static void fake_hash(const uint8_t salt[AUTH_SALT_LEN], const char *pass, uint8_t out[AUTH_HASH_LEN])
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < AUTH_SALT_LEN; i++) h = (h ^ salt[i]) * 16777619u;
    for (const char *p = pass; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    for (int i = 0; i < AUTH_HASH_LEN; i++) { h = (h ^ (uint32_t)i) * 16777619u; out[i] = (uint8_t)(h >> 24); }
}

static void set(authc_t *a, const char *user, const char *pass)
{
    uint8_t salt[AUTH_SALT_LEN], hash[AUTH_HASH_LEN];
    for (int i = 0; i < AUTH_SALT_LEN; i++) salt[i] = (uint8_t)(i * 7 + 3);
    fake_hash(salt, pass, hash);
    authc_set(a, user, salt, hash);
}

#define SEC 1000000LL
#define T1 "0123456789abcdef0123456789abcdef"
#define T2 "fedcba9876543210fedcba9876543210"

int main(void)
{
    authc_t a;
    memset(&a, 0, sizeof(a));

    /* ---- no login yet: the login is REQUIRED, so nothing may be changed ---- */
    CHECK("no login until one is created", !authc_enabled(&a));
    CHECK("no login yet: NUT writes refused with no USERNAME/PASSWORD",
          authc_nut_write(&a, NULL, NULL, 0, fake_hash) == AUTHC_NUT_DENIED);
    CHECK("no login yet: NUT writes refused whatever credentials a client volunteers",
          authc_nut_write(&a, "monuser", "secret", 0, fake_hash) == AUTHC_NUT_DENIED);
    CHECK("off: no login succeeds (there is nothing to log in to)",
          authc_check(&a, "", "", 0, fake_hash) != AUTHC_OK);
    CHECK("off: no token is valid", !authc_token_ok(&a, T1, 0));

    /* ---- what can be stored ---- */
    CHECK("shape: ordinary username and password", authc_shape_ok("admin", "hunter22"));
    CHECK("shape: no spaces (NUT args are words)", !authc_shape_ok("ad min", "hunter22"));
    CHECK("shape: no quotes", !authc_shape_ok("admin", "hun\"ter"));
    CHECK("shape: password of 3 refused", !authc_shape_ok("admin", "abc"));
    CHECK("shape: password of 7 refused", !authc_shape_ok("admin", "abcdefg"));
    CHECK("shape: password of 8 accepted", authc_shape_ok("admin", "abcdefgh"));
    CHECK("shape: password of 64 accepted",
          authc_shape_ok("admin", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    CHECK("shape: password of 65 refused",
          !authc_shape_ok("admin", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    CHECK("shape: username of 33 refused",
          !authc_shape_ok("uuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuuu", "hunter22"));
    CHECK("shape: empty username refused", !authc_shape_ok("", "hunter22"));

    /* ---- on ---- */
    set(&a, "admin", "hunter22");
    CHECK("on once set", authc_enabled(&a));
    CHECK("right username and password", authc_check(&a, "admin", "hunter22", 0, fake_hash) == AUTHC_OK);
    CHECK("wrong password", authc_check(&a, "admin", "hunter2", 0, fake_hash) == AUTHC_BAD);
    CHECK("wrong username", authc_check(&a, "root", "hunter22", 0, fake_hash) == AUTHC_BAD);
    CHECK("username is a prefix: refused", authc_check(&a, "admi", "hunter22", 0, fake_hash) == AUTHC_BAD);
    CHECK("username with extra: refused", authc_check(&a, "admin2", "hunter22", 0, fake_hash) == AUTHC_BAD);
    CHECK("NULL password: refused", authc_check(&a, "admin", NULL, 0, fake_hash) == AUTHC_BAD);

    /* ---- lockout ---- */
    set(&a, "admin", "hunter22");
    for (int i = 0; i < AUTH_MAX_FAILS - 1; i++) authc_check(&a, "admin", "nope", 10 * SEC, fake_hash);
    CHECK("four wrong: still answering", authc_check(&a, "admin", "hunter22", 10 * SEC, fake_hash) == AUTHC_OK);
    for (int i = 0; i < AUTH_MAX_FAILS - 1; i++) authc_check(&a, "admin", "nope", 11 * SEC, fake_hash);
    CHECK("a success resets the count", authc_check(&a, "admin", "nope", 11 * SEC, fake_hash) == AUTHC_BAD);
    CHECK("fifth wrong in a row locks", authc_check(&a, "admin", "hunter22", 12 * SEC, fake_hash) == AUTHC_LOCKED);
    CHECK("locked: even the right password is refused",
          authc_check(&a, "admin", "hunter22", 70 * SEC, fake_hash) == AUTHC_LOCKED);
    CHECK("locked: NUT writes refused too",
          authc_nut_write(&a, "admin", "hunter22", 70 * SEC, fake_hash) == AUTHC_NUT_DENIED);
    CHECK("unlocked after a minute", authc_check(&a, "admin", "hunter22", 72 * SEC, fake_hash) == AUTHC_OK);

    /* ---- tokens ---- */
    set(&a, "admin", "hunter22");
    authc_issue(&a, T1, 0);
    CHECK("issued token valid", authc_token_ok(&a, T1, 1 * SEC));
    CHECK("other token not valid", !authc_token_ok(&a, T2, 1 * SEC));
    CHECK("token of wrong length not valid", !authc_token_ok(&a, "0123", 1 * SEC));
    CHECK("NULL token not valid", !authc_token_ok(&a, NULL, 1 * SEC));
    CHECK("used within 12 h: still valid", authc_token_ok(&a, T1, 11 * 3600 * SEC));
    CHECK("use refreshes it", authc_token_ok(&a, T1, 22 * 3600 * SEC));
    CHECK("idle over 12 h: expired", !authc_token_ok(&a, T1, 35 * 3600 * SEC));
    authc_issue(&a, T1, 0);
    authc_revoke(&a, T1);
    CHECK("logged out token not valid", !authc_token_ok(&a, T1, 1 * SEC));
    authc_issue(&a, T1, 0);
    set(&a, "admin", "newpass1");
    CHECK("changing the login logs everyone out", !authc_token_ok(&a, T1, 1 * SEC));
    authc_issue(&a, T1, 0);
    authc_set(&a, "", NULL, NULL);
    CHECK("turning it off: off, and tokens gone", !authc_enabled(&a) && !authc_token_ok(&a, T1, 1 * SEC));
    set(&a, "admin", "hunter22");
    authc_issue(&a, T1, 1 * SEC);
    authc_issue(&a, "11111111111111111111111111111111", 2 * SEC);
    authc_issue(&a, "22222222222222222222222222222222", 3 * SEC);
    authc_issue(&a, "33333333333333333333333333333333", 4 * SEC);
    authc_issue(&a, T2, 5 * SEC);
    CHECK("a fifth login evicts the oldest", !authc_token_ok(&a, T1, 6 * SEC) && authc_token_ok(&a, T2, 6 * SEC));

    /* ---- NUT, in upsd's words and order ---- */
    set(&a, "admin", "hunter22");
    CHECK("NUT: no USERNAME -> USERNAME-REQUIRED",
          authc_nut_write(&a, NULL, NULL, 0, fake_hash) == AUTHC_NUT_USERNAME_REQUIRED);
    CHECK("NUT: USERNAME but no PASSWORD -> PASSWORD-REQUIRED",
          authc_nut_write(&a, "admin", NULL, 0, fake_hash) == AUTHC_NUT_PASSWORD_REQUIRED);
    CHECK("NUT: wrong password -> ACCESS-DENIED",
          authc_nut_write(&a, "admin", "nope", 0, fake_hash) == AUTHC_NUT_DENIED);
    CHECK("NUT: Synology's monuser/secret -> ACCESS-DENIED",
          authc_nut_write(&a, "monuser", "secret", 0, fake_hash) == AUTHC_NUT_DENIED);
    CHECK("NUT: right credentials -> allowed",
          authc_nut_write(&a, "admin", "hunter22", 0, fake_hash) == AUTHC_NUT_ALLOW);

    /* ---- another page posting to the board ---- */
    CHECK("origin: none (curl, scripts) allowed", authc_origin_ok(NULL, "192.0.2.5"));
    CHECK("origin: the board's own page allowed", authc_origin_ok("http://192.0.2.5", "192.0.2.5"));
    CHECK("origin: by .local name allowed",
          authc_origin_ok("http://ups-esp32-ab12.local", "ups-esp32-ab12.local"));
    CHECK("origin: setup AP allowed", authc_origin_ok("http://192.168.4.1", "192.168.4.1"));
    CHECK("origin: another site refused", !authc_origin_ok("https://evil.example", "192.0.2.5"));
    CHECK("origin: another LAN host refused", !authc_origin_ok("http://192.0.2.9", "192.0.2.5"));
    CHECK("origin: host as a prefix refused", !authc_origin_ok("http://192.0.2.50", "192.0.2.5"));
    CHECK("origin: other port refused", !authc_origin_ok("http://192.0.2.5:8080", "192.0.2.5"));
    CHECK("origin: null (sandboxed page) refused", !authc_origin_ok("null", "192.0.2.5"));
    CHECK("origin: present but no Host refused", !authc_origin_ok("http://192.0.2.5", ""));

    return fails ? 1 : 0;
}
'''


def source_checks():
    fails = 0
    def check(label, cond, detail=""):
        nonlocal fails
        print(("  ok   " if cond else "  FAIL ") + label + ("" if cond else f"  {detail}"))
        fails += 0 if cond else 1

    web = open(os.path.join(FW, "components", "webui", "webui.c")).read()
    posts = re.findall(r'\.method = HTTP_POST, \.handler = (\w+)', web)
    check("the web UI has POST routes to check", len(posts) >= 8, posts)
    exempt = {"post_auth_login", "post_auth_logout"}
    # Creating the FIRST login needs no login (there is none); changing it does.
    special = {"post_auth_set": "if (auth_enabled() ? !may_write(req) : !origin_ok(req)) return ESP_OK;"}
    for h in posts:
        m = re.search(r'static esp_err_t %s\(httpd_req_t \*req\)\s*\{\s*(?:/\*[^\n]*\*/\s*)?([^\n]*)' % h, web)
        first = m.group(1).strip() if m else ""
        if h in exempt:
            continue
        want = special.get(h, "if (!may_write(req)) return ESP_OK;")
        check(f"{h} starts with its write check", first == want, first or "handler not found")
    # Every read of the page's data is behind the login too, once one exists.
    # Open on purpose: the page shell, /metrics (as NUT reads are), the captive
    # redirect.
    gets = re.findall(r'\.method = HTTP_GET,\s*\.handler = (\w+)', web)
    open_reads = {"get_root", "get_metrics", "get_catchall"}
    check("the web UI has GET routes to check", len(gets) >= 6, gets)
    for h in gets:
        if h in open_reads:
            continue
        m = re.search(r'static esp_err_t %s\(httpd_req_t \*req\)\s*\{\s*([^\n]*)' % h, web)
        first = m.group(1).strip() if m else ""
        check(f"{h} starts with may_read()", first == "if (!may_read(req)) return ESP_OK;",
              first or "handler not found")
    mw = re.search(r'static bool may_write\(httpd_req_t \*req\)\s*\{.*?\n\}', web, re.S).group(0)
    check("with no login yet, writes are refused outside setup",
          "if (!auth_enabled())" in mw and "NETMGR_AP_PROVISIONING" in mw and "create a login" in mw)
    aset = re.search(r'static esp_err_t post_auth_set\(.*?\n\}', web, re.S).group(0)
    check("the login cannot be turned off", "a login is required" in aset)
    login = re.search(r'static esp_err_t post_auth_login\(.*?\n\}', web, re.S).group(0)
    check("login refuses requests from another page", "auth_origin_ok" in login)
    check("the 401 carries no WWW-Authenticate (no browser prompt)",
          not re.search(r'set_hdr\([^)]*WWW-Authenticate', web))

    nut = open(os.path.join(FW, "components", "nut_server", "nut_server.c")).read()
    inst = re.search(r'"INSTCMD"\) && argc >= 3\).*?\} else if', nut, re.S).group(0)
    setv = re.search(r'"SET"\) && argc >= 5.*?\} else \{', nut, re.S).group(0)
    check("NUT INSTCMD goes through nut_may_write()", "nut_may_write(c)" in inst)
    check("NUT SET VAR goes through nut_may_write()", "nut_may_write(c)" in setv)
    check("NUT never logs a PASSWORD line", 'PASSWORD (hidden)' in nut)

    auth = open(os.path.join(AUTH, "auth.c")).read()
    net = open(os.path.join(FW, "components", "netmgr", "netmgr.c")).read()
    ans = re.search(r'#define NVS_NS\s+"(\w+)"', auth).group(1)
    nns = re.search(r'#define NVS_NS\s+"(\w+)"', net).group(1)
    reset = re.search(r'esp_err_t netmgr_factory_reset\(void\)\s*\{.*?\n\}', net, re.S)
    check("the login lives in the namespace the BOOT reset erases",
          ans == nns and reset and "nvs_erase_all" in reset.group(0), (ans, nns))
    return fails


def main():
    with tempfile.TemporaryDirectory() as td:
        c = os.path.join(td, "t.c")
        open(c, "w").write(TESTS)
        exe = os.path.join(td, "t")
        r = subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            "-I", os.path.join(AUTH, "include"), "-o", exe, c,
                            os.path.join(AUTH, "auth_core.c")],
                           capture_output=True, text=True)
        if r.returncode:
            print("  FAIL auth_core.c does not compile on the host:")
            print(r.stderr.strip()[:1500])
            return 1
        core = subprocess.run([exe]).returncode
    wiring = source_checks()
    ok = core == 0 and wiring == 0
    print(f"\n{'PASS' if ok else 'FAIL'}: optional login")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
