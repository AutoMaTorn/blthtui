# blttui

A lightweight, fast, nmtui-style TUI for managing Bluetooth on Linux.

Built in C on top of **BlueZ** (via `sd-bus` from libsystemd) with a **newt**
interface — the same widget toolkit NetworkManager's `nmtui` uses.

## Features

- Toggle the Bluetooth adapter on/off
- Scan for nearby devices
- Connect / disconnect (Enter on a device)
- Pair and remove devices
- Interactive pairing: PIN / passkey entry and confirmation dialogs
  (registers as a BlueZ `org.bluez.Agent1`)
- Live list, updated straight from BlueZ signals (device added/removed,
  connected, RSSI…) — not just a timer
- Paired (`+`) and connected (`*`) markers

## Build

Requires Debian/Ubuntu packages:

```sh
sudo apt install build-essential libnewt-dev libsystemd-dev
make
```

## Run

```sh
./blttui
```

`bluetoothd` must be running and your user should be in the `bluetooth`
group (root is not required).

## Keys

| Key            | Action                          |
|----------------|---------------------------------|
| Up / Down      | Move in the device list         |
| Enter          | Connect / disconnect selected   |
| Tab            | Move between list and buttons    |
| F10 / Esc / q  | Quit                            |

## Installing on other machines

### Build a .deb (recommended)

```sh
sudo apt install build-essential debhelper libnewt-dev libsystemd-dev pkg-config
dpkg-buildpackage -b -us -uc          # produces ../blttui_<ver>_amd64.deb
```

Copy the resulting `.deb` to any Debian/Ubuntu machine and install it — `apt`
pulls in the runtime dependencies (`libnewt0.52`, `libsystemd0`, `bluez`)
automatically:

```sh
sudo apt install ./blttui_0.1.0_amd64.deb
```

Remove with `sudo apt remove blttui`.

### Hosting your own apt repository

To let machines `apt install blttui` from a URL, publish the `.deb` with a
tool like `reprepro` or `aptly` on a web server, sign it with a GPG key, and on
each client:

```sh
echo "deb [signed-by=/usr/share/keyrings/blttui.gpg] https://you.example/apt ./" \
    | sudo tee /etc/apt/sources.list.d/blttui.list
sudo apt update && sudo apt install blttui
```

For Ubuntu specifically, a Launchpad **PPA** builds the `.deb` from source for
users. Getting into the official Debian archive requires an ITP bug and a
sponsoring maintainer.

## Debugging

**Log mode.** Set `BLTTUI_LOG` to a file path to record every D-Bus call,
its result and any error text (no-op when unset):

```sh
BLTTUI_LOG=/tmp/blttui.log ./blttui   # then, in another terminal:
tail -f /tmp/blttui.log
```

**Sanitizer build.** `make debug` produces `./blttui-debug` with
AddressSanitizer + UndefinedBehaviorSanitizer (catches leaks and bad memory
use in the manual sd-bus message handling).

**Headless harness.** `make test` produces `./bttest`, which drives the BlueZ
layer (`bt.c`) without the TUI — ideal under `gdb` or ASan, since newt won't
fight for the terminal:

```sh
make test && ./bttest
```

**Live D-Bus tracing** (no build needed):

```sh
busctl monitor org.bluez            # watch all BlueZ traffic
busctl introspect org.bluez /org/bluez/hci0
```

> `make` and `make debug` link `-lnewt`, so they need `libnewt-dev`.
> `make test` does not link newt and builds without it.

## Layout

```
src/
  device.h   device model
  bt.c/.h    BlueZ access over sd-bus
  ui.c/.h    newt interface + main loop
  log.c/.h   optional file logger (BLTTUI_LOG)
  main.c     entry point
tools/
  bttest.c   headless debug harness (make test)
```
