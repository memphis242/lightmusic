#!/bin/sh
# Runs test_mpris on a private session bus. Exit 77 (skip) without dbus-run-session.
command -v dbus-run-session >/dev/null 2>&1 || { echo "skip: no dbus-run-session"; exit 77; }
exec dbus-run-session -- "$@"
