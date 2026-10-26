#!/bin/sh
# fetch the sqlite amalgamation (public domain) into third_party/sqlite, if the system has no libsqlite3-dev
set -e
cd "$(dirname "$0")"
V=3460100
mkdir -p dl sqlite
test -f sqlite/sqlite3.c && exit 0
curl -sfLo dl/sq.zip https://www.sqlite.org/2024/sqlite-amalgamation-$V.zip
unzip -q -o dl/sq.zip -d dl
cp dl/sqlite-amalgamation-$V/sqlite3.c dl/sqlite-amalgamation-$V/sqlite3.h sqlite/
