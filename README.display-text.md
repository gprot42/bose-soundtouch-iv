# Custom Text on the Wave SoundTouch IV Display

Put **arbitrary text** on the Wave SoundTouch IV front display from your Mac or
scripts — not just the track names, `SETUP SEE INSTRUCTIONS`, and volume the
firmware shows on its own.

> Unsupported / firmware-specific. This drives an internal diagnostic command
> over the device's CLI. Verified on a live Wave SoundTouch IV running firmware
> **27.00.06** (build codename **`triode`**). Behaviour on other models or
> firmware may differ.

---

## TL;DR

```sh
# One-off message (goes into the title line)
python3 dlna-sender/send-display-text.py "HELLO WORLD"

# Multiple lines
python3 dlna-sender/send-display-text.py --title "DINNER" --artist "READY"

# Keep it up ~60s against now-playing refreshes
python3 dlna-sender/send-display-text.py --title "MEETING" --hold 60

# Hold until you press Ctrl+C
python3 dlna-sender/send-display-text.py --title "STAY" --hold 0

# Blank the display
python3 dlna-sender/send-display-text.py --clear
```

Set the target device with `--ip 192.168.0.119` or `BOSE_IP=192.168.0.119`.

---

## Why this is non-obvious

The display is **not** on the Linux box you SSH into. The pedestal runs
`BoseApp` and the SoundTouch APIs (`:8090` / `:17000`); the Wave console (top
unit) owns the actual screen. They communicate over an internal ABL bus
(`ABLServer` → `/dev/abl0`). There is **no** public REST endpoint like
`/display` or `/message` on `:8090`.

Earlier attempts using `display set countdown <msg>` returned `OK` but showed
nothing: that command targets an **OLED** countdown screen used by *other*
SoundTouch variants (firmware codenames Lisa / Seine), and on `triode` the
countdown screen is only activated by factory-default / Wi-Fi-AP key combos —
not by the CLI.

The Wave's front display is a **VFD** driven by `ABLServer`'s *remote display*
commands, which **are** reachable from the CLI.

---

## The mechanism

`ABLServer` keeps a global "remote display" buffer. You set fields in it, then
push the buffer to the VFD. Both steps are CLI commands registered with
`CLIServer`, reachable over the telnet CLI on **port 17000** (the same CLI used
for `sys volume … updateDisplay`).

```
abl rdset <field> "<text>"     # set a field in the remote-display buffer
abl rdsend state               # push the buffer to the VFD
```

### `rdset` field grammar (from firmware)

```
rdset <field> <param1> <param2>
  source | station | title | artist | album   → param1 = "test string"   (free text)
  state                                        → param1 = static text string  ("" intended to clear)
  preset                                       → param1 = number 1-6
  repeat | shuffle | paused | buffering        → no param1 (toggles)
  timeout                                      → param1 = temp text, param2 = seconds
```

| Field | Use | Status |
|-------|-----|--------|
| `title`, `artist`, `album`, `source`, `station` | Now-playing text lines | **Works** — confirmed on screen |
| `state` | Dedicated static message line | **Works** |
| `timeout` | Auto-expiring timed message | Present in firmware, but **did not populate via the CLI** in testing — not exposed by the script |
| `preset`, `repeat`, `shuffle`, `paused`, `buffering` | Status flags/icons | Not text |

### Clearing a field

Set it to a **single space** (`" "`). An empty string (`""`) is parsed as a
*missing* argument and silently ignored, so it does **not** clear the field.
The script's `--clear` blanks every field with `" "`.

### Persistence

The remote-display buffer is global server state, but the firmware repaints the
panel on the next now-playing / volume / source refresh, which overwrites your
text. Use `--hold SECS` (re-push every `--interval` seconds, default 3s) to keep
text visible; `--hold 0` runs until `Ctrl+C`.

---

## Manual example (telnet)

```sh
# Connect to the diagnostic CLI
nc 192.168.0.119 17000        # or: telnet 192.168.0.119 17000

abl rdset title "HELLO WORLD"
abl rdset artist "FROM THE CLI"
abl rdsend state
```

Each `rdset` echoes the full buffer; `rdsend state` replies `OK`.

---

## Script reference — `dlna-sender/send-display-text.py`

| Flag | Meaning |
|------|---------|
| `text` (positional) | Shorthand for `--title` |
| `--title / --artist / --album / --source / --station STR` | Set a now-playing text line |
| `--state STR` | Set the dedicated static message line |
| `--clear` | Blank all fields before setting |
| `--hold SECS` | Keep re-pushing; `0` = until Ctrl+C |
| `--interval SECS` | Re-push interval for `--hold` (default 3.0) |
| `--ip IP` | Target device (default `$BOSE_IP` or `192.168.0.119`) |

Python 3.8+, standard library only.

---

## Alternative: scrolling text while "playing"

If you want text that scrolls as if a track is playing, push audio via DLNA with
custom title/artist metadata using
[`send-to-bose.py`](dlna-sender/send-to-bose.py). The remote-display method above
is better for short, static, non-playback messages.
