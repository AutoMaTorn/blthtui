# bluetui

*[Русская версия](README.ru.md)*

A lightweight, fast, nmtui-style TUI for managing Bluetooth on Linux.

Built in C on top of **BlueZ** (via `sd-bus` from libsystemd) with a **newt**
interface — the same widget toolkit NetworkManager's `nmtui` uses. No Python,
no daemon of its own, no desktop environment required: it runs anywhere you
have a terminal and `bluetoothd`.

## Features

- Toggle the Bluetooth adapter on/off
- Scan for nearby devices
- Connect / disconnect (Enter on a device)
- Pair, trust and remove devices
- Interactive pairing: PIN / passkey entry and confirmation dialogs
  (registers as a BlueZ `org.bluez.Agent1`)
- Connect and pair run asynchronously — the interface stays live and shows the
  operation in progress instead of freezing
- Device details on demand: battery level (via `org.bluez.Battery1`), device
  type and service UUIDs
- Filter the list by name, or show only paired devices
- Adapter picker when more than one adapter is present
- Live list, updated straight from BlueZ signals (device added/removed,
  connected, RSSI…) rather than by re-enumerating on a timer; RSSI updates
  don't reorder rows under the cursor
- Paired (`+`) and connected (`*`) markers; connected and paired devices sort
  to the top, the rest by signal strength
- Device names in any language render correctly (UTF-8-aware, width-correct)
- PIN / passkey entry is hidden (password-style) and validated
- Clean exit on Ctrl-C / SIGTERM: stops the discovery it started and
  unregisters the pairing agent instead of leaving both behind on the bus

## Build

Requires Debian/Ubuntu packages:

```sh
sudo apt install build-essential libnewt-dev libsystemd-dev pkg-config
make
```

## Run

```sh
./bluetui
```

`bluetoothd` must be running and your user should be in the `bluetooth`
group (root is not required).

## Keys

| Key            | Action                              |
|----------------|-------------------------------------|
| Up / Down      | Move in the device list             |
| Enter          | Connect / disconnect selected       |
| `p`            | Pair selected                       |
| `t`            | Toggle trust on selected            |
| `d`            | Show device details                 |
| `r`            | Remove selected                     |
| `s`            | Start / stop scanning               |
| `/`            | Filter the list by name             |
| `o`            | Toggle showing only paired devices  |
| Tab            | Move between list and buttons       |
| F10 / Esc / q  | Quit                                |

## Installing on other machines

### Build a .deb (recommended)

```sh
sudo apt install build-essential debhelper libnewt-dev libsystemd-dev pkg-config
dpkg-buildpackage -b -us -uc          # produces ../bluetui_<ver>_amd64.deb
```

Copy the resulting `.deb` to any Debian/Ubuntu machine and install it — `apt`
pulls in the runtime dependencies (`libnewt0.52`, `libsystemd0`, `bluez`)
automatically:

```sh
sudo apt install ./bluetui_0.2.1_amd64.deb
```

Remove with `sudo apt remove bluetui`.

The build also produces `bluetui-dbgsym_<ver>_amd64.deb`. That package holds only
the debug symbols, split out of the binary by `dh_strip` and matched to it by
Build-ID. It is not needed to run bluetui — install it alongside the main package
when you want a readable backtrace out of `gdb` or a core dump.

## Debugging

**Log mode.** Set `BLUETUI_LOG` to a file path to record every D-Bus call,
its result and any error text (no-op when unset):

```sh
BLUETUI_LOG=/tmp/bluetui.log ./bluetui   # then, in another terminal:
tail -f /tmp/bluetui.log
```

**Sanitizer build.** `make debug` produces `./bluetui-debug` with
AddressSanitizer + UndefinedBehaviorSanitizer (catches leaks and bad memory
use in the manual sd-bus message handling).

**Headless harness.** `make test` produces `./bttest`, which drives the BlueZ
layer (`bt.c`) without the TUI — ideal under `gdb` or ASan, since newt won't
fight for the terminal. It is a smoke/debug harness, not a test suite: it
prints the adapter, its state and the device list, and asserts nothing.

```sh
make test && ./bttest
```

**Unit tests.** `make check` builds and runs `tests/test_strutil.c` — plain
asserts over the pure helpers (UTF-8 padding, device sorting, name filter,
passkey parsing). No D-Bus, no newt, no hardware needed, so it runs anywhere.

```sh
make check
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
  device.h     device model
  bt.c/.h      BlueZ access over sd-bus
  ui.c/.h      newt interface + main loop
  strutil.c/.h pure helpers: UTF-8 padding, sorting, filter, passkey parsing
  log.c/.h     optional file logger (BLUETUI_LOG)
  main.c       entry point
tools/
  bttest.c     headless debug harness (make test)
tests/
  test_strutil.c  unit tests for the pure helpers (make check)
debian/        Debian packaging (native 3.0, debhelper 13)
```

`bt.c` never touches the UI: the pairing agent reaches the interface only
through the `bt_agent_cb` callback struct. That is what lets the headless
harness build without newt.

## Known limitations

- The bus fd is watched for reading only; in the rare case sd-bus needs to
  write or has its own timeout, it is serviced on the 4 s fallback timer.
- A pairing dialog left unanswered for more than 180 s ends with a timeout.
- The device list is capped at 256 entries; a "list full" row is shown when it
  overflows.
- The adapter picker is offered only at startup — no live switching mid-session
  (a vanished adapter is re-acquired automatically).

## License

MIT — see [LICENSE](LICENSE).
