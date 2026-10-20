#!/bin/sh
# WASAPI Driver for ASIO - optional Authenticode signing of release files (0.8.0, G8). See SIGNING.txt.
# Copyright (C) 2026 The WASAPI Driver for ASIO contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Usage: sh scripts/sign.sh FILE...
#   Signs every FILE in place with osslsigncode (SHA-256 signature + RFC 3161 timestamp) when a PFX certificate is
#   configured, and does NOTHING (exit 0, one line printed) when it is not. Used by the release workflow for the
#   DLLs and EXEs and, through NSIS !finalize / !uninstfinalize, for the Setup and its uninstaller.
#
# Configuration (environment; in GitHub Actions they come from repository secrets / variables):
#   WB_SIGN_PFX_BASE64    the .pfx file encoded with base64 (secret). Not set -> no signing.
#   WB_SIGN_PFX_PASSWORD  its password (secret).
#   WB_SIGN_TIMESTAMP     RFC 3161 timestamp server URL (variable). Default: http://timestamp.digicert.com
#   WB_SIGN_NAME          program name shown by Windows (default: WASAPI Driver for ASIO)
#   WB_SIGN_URL           project URL shown by Windows (optional)
# Azure Trusted Signing does not use this script: it signs on a Windows runner with signtool and the Trusted
# Signing dlib (release.yml job "package-trusted").
#
# The certificate and its password are written to temporary files readable only by the current user (umask 077,
# chmod 600) and removed on exit; they are never printed or passed on a command line. A signing failure is an error (exit 1): an unsigned file is never published as signed.
set -eu
if [ -z "${WB_SIGN_PFX_BASE64:-}" ]; then
    echo "sign.sh: no certificate configured, files left unsigned: $*"
    exit 0
fi
command -v osslsigncode >/dev/null 2>&1 || { echo "sign.sh: osslsigncode is not installed" >&2; exit 1; }
TS="${WB_SIGN_TIMESTAMP:-http://timestamp.digicert.com}"
NAME="${WB_SIGN_NAME:-WASAPI Driver for ASIO}"
umask 077
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM
printf '%s' "$WB_SIGN_PFX_BASE64" | base64 -d > "$WORK/cert.pfx"
# 0.9.1 (fix 10): the password goes through a 0600 file (-readpass), never on the command line, where other
# processes of the machine could read it (ps, /proc/<pid>/cmdline). printf is a shell builtin: no child process
# sees the value either.
printf '%s' "${WB_SIGN_PFX_PASSWORD:-}" > "$WORK/pass"
chmod 600 "$WORK/cert.pfx" "$WORK/pass"
# One signature (the optional -i URL is added only when set; the password never appears in the log).
sign_one() {
    if [ -n "${WB_SIGN_URL:-}" ]; then
        osslsigncode sign -pkcs12 "$WORK/cert.pfx" -readpass "$WORK/pass" -h sha256 -n "$NAME" -i "$WB_SIGN_URL" -ts "$TS" -in "$1" -out "$2" >/dev/null
    else
        osslsigncode sign -pkcs12 "$WORK/cert.pfx" -readpass "$WORK/pass" -h sha256 -n "$NAME" -ts "$TS" -in "$1" -out "$2" >/dev/null
    fi
}
for f in "$@"; do
    [ -f "$f" ] || { echo "sign.sh: missing file: $f" >&2; exit 1; }
    rm -f "$WORK/signed"
    # Two attempts: public timestamp servers occasionally time out.
    sign_one "$f" "$WORK/signed" || { sleep 5; rm -f "$WORK/signed"; sign_one "$f" "$WORK/signed"; }
    osslsigncode verify -in "$WORK/signed" >/dev/null 2>&1 || echo "sign.sh: note: verify could not build the chain for $(basename "$f") (normal without the root store)"
    cat "$WORK/signed" > "$f"
    echo "sign.sh: signed $(basename "$f")"
done
