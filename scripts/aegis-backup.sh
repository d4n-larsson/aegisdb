#!/usr/bin/env bash
#
# Off-box backup for AegisDB.
#
# Takes a consistent online snapshot (the admin `snapshot` wire op, so the
# server keeps serving), packages it as a tarball, and ships it off the box with
# a transport command you supply — AegisDB stays dependency-free and provider-
# agnostic, the same way it delegates TLS to a proxy. Restart + a durable volume
# already survive a process crash; this is what survives losing the whole box.
#
# Requires: bash (uses /dev/tcp — no nc), tar, and read access to the data
# volume (the snapshot lands under <data-dir>/snapshots/). Run it from host cron,
# `docker exec`, or the optional compose `backup` sidecar.
#
# Configure via environment (defaults in parens):
#   AEGIS_HOST                (127.0.0.1)   server host
#   AEGIS_PORT                (9470)        server port
#   AEGIS_AUTH_TOKEN          ()            admin token, if auth is enabled
#   AEGIS_DATA_DIR            (./data)      data dir (holds snapshots/); the
#                                           volume path when run in a sidecar
#   AEGIS_BACKUP_DIR          ($DATA_DIR/backups)  local staging for tarballs
#   AEGIS_BACKUP_UPLOAD_CMD   ()            transport; "{}" is replaced with the
#                                           tarball path, e.g.
#                                             'aws s3 cp {} s3://bucket/aegis/'
#                                             'rclone copy {} remote:aegis'
#                                             'rsync -a {} backups:/aegis/'
#                                           If unset, the tarball is kept locally
#                                           only (NOT off-box) and a warning is
#                                           logged.
#   AEGIS_BACKUP_RETAIN       (7)           local tarballs to keep (prune older)
#   AEGIS_KEEP_SERVER_SNAPSHOT(0)           1 = leave the server-side snapshot
#                                           dir in place after packaging
#
# Exit non-zero on any failure so cron / the sidecar surfaces it.
set -euo pipefail

HOST=${AEGIS_HOST:-127.0.0.1}
PORT=${AEGIS_PORT:-9470}
TOKEN=${AEGIS_AUTH_TOKEN:-}
DATA_DIR=${AEGIS_DATA_DIR:-./data}
BACKUP_DIR=${AEGIS_BACKUP_DIR:-$DATA_DIR/backups}
UPLOAD_CMD=${AEGIS_BACKUP_UPLOAD_CMD:-}
RETAIN=${AEGIS_BACKUP_RETAIN:-7}
KEEP_SERVER_SNAPSHOT=${AEGIS_KEEP_SERVER_SNAPSHOT:-0}

log() { printf '%s aegis-backup: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" >&2; }
die() { log "ERROR: $*"; exit 1; }

# JSON-escape a string for a "..." value: escape backslash and double-quote, and
# reject control characters (which can't appear in a valid token and would break
# the single-line request). Kept dependency-free — no jq — matching the script's
# bash+tar-only contract. Never logged.
json_escape() {
    local s=$1
    case $s in
        *[[:cntrl:]]*) die "AEGIS_AUTH_TOKEN contains control characters" ;;
    esac
    s=${s//\\/\\\\}   # backslash first
    s=${s//\"/\\\"}   # then double-quote
    printf '%s' "$s"
}

command -v tar >/dev/null 2>&1 || die "tar not found"

TS=$(date -u +%Y%m%dT%H%M%SZ)
NAME="aegis-$TS"                     # snapshot name = single path component
SNAP_DIR="$DATA_DIR/snapshots/$NAME"

# --- 1. trigger a consistent online snapshot over the wire (bash /dev/tcp) ----
tokfield=""
[ -n "$TOKEN" ] && tokfield=",\"token\":\"$(json_escape "$TOKEN")\""
req="{\"operation\":\"snapshot\",\"name\":\"$NAME\"$tokfield}"

log "requesting snapshot '$NAME' from $HOST:$PORT"
resp=""
if ! exec 3<>"/dev/tcp/$HOST/$PORT"; then die "cannot connect to $HOST:$PORT"; fi
printf '%s\n' "$req" >&3
IFS= read -r -t 60 resp <&3 || die "no response from server (timeout)"
exec 3>&- 3<&-

case "$resp" in
    *'"ok":true'*) : ;;
    *) die "snapshot failed: $resp" ;;
esac
[ -d "$SNAP_DIR" ] || die "snapshot dir not found at $SNAP_DIR (is AEGIS_DATA_DIR correct / the volume mounted?)"
log "snapshot written: $SNAP_DIR"

# --- 2. package the snapshot into a single tarball ----------------------------
mkdir -p "$BACKUP_DIR"
TARBALL="$BACKUP_DIR/$NAME.tar.gz"
tar -czf "$TARBALL" -C "$DATA_DIR/snapshots" "$NAME"
log "packaged: $TARBALL ($(du -h "$TARBALL" | cut -f1))"

# The tarball supersedes the raw server-side snapshot dir; drop it so the data
# volume doesn't accumulate snapshots (unless explicitly told to keep it).
if [ "$KEEP_SERVER_SNAPSHOT" != "1" ]; then
    rm -rf "$SNAP_DIR"
fi

# --- 3. ship it off-box via the supplied transport ----------------------------
if [ -n "$UPLOAD_CMD" ]; then
    cmd=${UPLOAD_CMD//\{\}/$TARBALL}   # substitute {} -> tarball path
    # Don't log $cmd — the upload command may embed a credential (an S3 key, an
    # rclone token). Log only what is being shipped.
    log "uploading $TARBALL off-box"
    if ! sh -c "$cmd"; then die "upload command failed"; fi
    log "upload ok"
else
    log "WARNING: AEGIS_BACKUP_UPLOAD_CMD unset — tarball kept LOCAL only, not off-box"
fi

# --- 4. local retention: keep the newest N tarballs ---------------------------
if [ "$RETAIN" -gt 0 ]; then
    # shellcheck disable=SC2012  # names are timestamped; ls -t ordering is fine
    ls -1t "$BACKUP_DIR"/aegis-*.tar.gz 2>/dev/null | tail -n +"$((RETAIN + 1))" \
        | while IFS= read -r old; do log "pruning old backup: $old"; rm -f "$old"; done
fi

log "backup complete: $NAME"