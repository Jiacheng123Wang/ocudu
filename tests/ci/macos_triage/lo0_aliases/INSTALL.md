# Persistent lo0 aliases (LaunchDaemon)

macOS drops the `lo0` aliases required by the test suite on every reboot (see
[../README.md](../README.md), "Test-host prerequisites"). Without them the gateway/f1u/cu_up/e2
tests fail en masse with `Can't assign requested address` (18 cases on the 2026-09-01 scan). This
bundle installs a LaunchDaemon that re-adds the aliases at boot, so `make test` never hits that
failure class again.

## Install (once)

```sh
sudo cp add_lo0_aliases.sh /usr/local/sbin/add_lo0_aliases.sh
sudo chmod 755 /usr/local/sbin/add_lo0_aliases.sh
sudo cp com.ocudu.lo0-test-aliases.plist /Library/LaunchDaemons/
sudo launchctl load -w /Library/LaunchDaemons/com.ocudu.lo0-test-aliases.plist

# Apply immediately (no reboot needed):
sudo launchctl kickstart -k system/com.ocudu.lo0-test-aliases
ifconfig lo0 | grep "inet 127"
```

## Verify

`ifconfig lo0` lists `127.0.0.2`, `127.0.0.3`, `127.0.1.1` and `127.0.0.101` in addition to
`127.0.0.1`. The daemon logs to `/var/log/ocudu-lo0-aliases.log`. The script is idempotent, so
re-running it (or the daemon) never duplicates an alias.

## Uninstall

```sh
sudo launchctl unload -w /Library/LaunchDaemons/com.ocudu.lo0-test-aliases.plist
sudo rm /Library/LaunchDaemons/com.ocudu.lo0-test-aliases.plist /usr/local/sbin/add_lo0_aliases.sh
```

Aliases added manually stay until the next reboot; uninstalling the daemon does not remove them.
