#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The USB serial `clear-login` command, compiled from main.c and run on the host.

It is the only way to recover a board whose password is forgotten without
losing its Wi-Fi, so it must work -- and it must not fire by accident: a
terminal's echo, boot-log noise or a long garbage line cannot clear a login.
The function is cut out of firmware/main/main.c verbatim and compiled with
stand-ins for the UART and the login store, then fed input.
"""
import os, re, subprocess, sys, tempfile, pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
src = (ROOT / "firmware/main/main.c").read_text()
m = re.search(r"static void serial_cmd_task\(void \*arg\)\n\{.*?\n\}\n", src, re.S)
if not m:
    sys.exit("FAIL: serial_cmd_task not found in main.c")
if not re.search(r'xTaskCreate\(serial_cmd_task,', src):
    sys.exit("FAIL: serial_cmd_task is never started")

harness = r'''
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
typedef int esp_err_t; typedef int uart_port_t;
#define ESP_OK 0
#define CONFIG_ESP_CONSOLE_UART_NUM 0
#define portMAX_DELAY 0
#define ESP_LOGW(t, ...) ((void)0)
#define ESP_LOGI(t, ...) ((void)0)
static const char *TAG = "t";
static const char *esp_err_to_name(esp_err_t e) { (void)e; return "err"; }
static void vTaskDelete(void *h) { (void)h; exit(3); }
static int uart_driver_install(int p, int a, int b, int c, void *d, int e) { return 0; }
static const char *in; static size_t pos, len; static int clears;
static int uart_read_bytes(uart_port_t p, uint8_t *c, int n, int w) {
    if (pos >= len) { printf("%d\n", clears); exit(0); }   /* input exhausted: report */
    *c = (uint8_t)in[pos++]; return 1;
}
static esp_err_t auth_set_credentials(const char *u, const char *p) {
    if (u || p) { printf("called with a login, not a clear\n"); exit(2); }
    clears++; return ESP_OK;
}
''' + m.group(0) + r'''
int main(int argc, char **argv) { in = argv[1]; len = strlen(in); serial_cmd_task(NULL); return 1; }
'''
cases = [  # (input, clears expected, why)
    ("clear-login\n", 1, "the command"),
    ("clear-login\r\n", 1, "CRLF from a terminal counts once"),
    ("clear-login\r", 1, "a bare CR ends the line too"),
    ("clear-login", 0, "no line ending yet: not acted on"),
    ("Clear-Login\n", 0, "case matters"),
    (" clear-login\n", 0, "leading space: not a whole-line match"),
    ("clear-login now\n", 0, "trailing text"),
    ("I (1234) main: clear-login\n", 0, "a log line that mentions it"),
    ("x" * 40 + "clear-login\n", 0, "overlong line ending in it"),
    ("x" * 40 + "\nclear-login\n", 1, "recovers after an overlong line"),
    ("clear-login\nclear-login\n", 2, "each line is its own command"),
]
with tempfile.TemporaryDirectory() as d:
    c, exe = os.path.join(d, "t.c"), os.path.join(d, "t")
    open(c, "w").write(harness)
    r = subprocess.run(["cc", "-std=c11", "-Wall", "-Wno-unused-function", "-Wno-unused-parameter",
                        "-o", exe, c], capture_output=True, text=True)
    if r.returncode:
        sys.exit("FAIL: does not compile on the host:\n" + r.stderr)
    fails = 0
    for text, want, why in cases:
        r = subprocess.run([exe, text], capture_output=True, text=True)
        got = r.stdout.strip()
        ok = r.returncode == 0 and got == str(want)
        print(f"{'ok  ' if ok else 'FAIL'} {why}: {got or r.returncode} clear(s), want {want}")
        fails += not ok
sys.exit(1 if fails else 0)
