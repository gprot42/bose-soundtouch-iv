# BosMan WiFi Guide

This guide explains how to put a **Bose Wave SoundTouch Music System IV** (SoundTouch “4”) into **WiFi setup mode** so it broadcasts its own SSID, how to configure your phone (including **GrapheneOS**), and how to use the **BosMan** app.

---

## ⭐ START HERE: "BosMan can't connect to the Bose" — fix in order

If the app won't connect, **the speaker is almost always not actually in setup mode.** Being connected to the `Bose Wave ST (…)` WiFi is **not** proof of setup mode — the speaker can broadcast that WiFi while its setup server is dead. Work through these steps **in order** and stop as soon as it connects.

> **The golden rule:** the speaker is only ready when its front display shows **`SETUP SEE INSTRUCTIONS`** and the **Wi‑Fi light is SOLID AMBER**. If you don't see *both*, no app and no amount of phone fiddling will work — fix the speaker first.

### Step 1 — Put the speaker into setup mode
- Press & **hold the Control button on the BACK of the pedestal for ~3 seconds**, then release.
- ✅ **Success looks like:** Wi‑Fi light **solid amber** **AND** display reads **`SETUP SEE INSTRUCTIONS`**.
- ❌ If you do **not** see both of those → go to **Step 4** (the firmware is likely hung; reset it).

### Step 2 — Connect the phone to the speaker's WiFi
- Join the `Bose Wave ST (…)` network. Tap **"Stay connected"** if the phone warns about no internet.
- (Optional sanity check: the phone should get an IP like `192.0.2.x` or `192.168.1.x`.)

### Step 3 — In BosMan, use the WiFi Setup panel
- Use the **WiFi Setup panel** — **NOT** the "Search devices" / "Connect by IP" buttons.
  - *Why:* "Search" looks for an already‑configured speaker on port 8090, which a speaker **in setup mode does not run yet.** It will always find nothing during setup. The WiFi Setup panel is the only correct path for first‑time provisioning.
- Pick your home WiFi from the list, enter the password, submit. The speaker reboots onto your network and the `Bose Wave ST` AP disappears.

### Step 4 — Speaker won't enter setup mode / still no connection → reset the pedestal
Do these **in order**, re‑checking Step 1 after each one:

1. **Soft restart:** hold the pedestal **Control** button **> 10 seconds**, release. Wait for it to come back (light returns to solid white). Retry Step 1.
2. **Hard reset (clears WiFi — use this for a hung pedestal):**
   - **Pull the AC power cord out of the wall outlet** (or out of the back of the unit) so the system goes **completely dark**. ⚠️ This means the **physical power cord** — *not* the power/standby button on the remote (standby keeps the pedestal powered and will NOT work).
   - Press and **hold the Control button on the back of the pedestal**.
   - **While you are still holding Control, plug the power cord back into the wall.**
   - Keep holding Control for **~5 more seconds**, then release.
   - Retry Step 1.
3. Still nothing? See the full [Factory reset / setup recovery](#factory-reset--setup-recovery-wave-soundtouch-series-iv) section.

> ⚠️ **Do NOT use `RESET ALL` from the remote for a WiFi problem.** That resets the *Wave radio/clock/presets*, **not** the WiFi pedestal. It won't help and you'll lose your presets. The WiFi pedestal is only reset with the **Control button on the back**.

### Still stuck? It's the speaker, not the app
If you've reached `SETUP SEE INSTRUCTIONS` + solid amber, the phone is on the `Bose Wave ST` WiFi, and it *still* won't connect, the speaker is genuinely not serving its setup page. Re‑do the **hard reset** (Step 4.2). To confirm whether the speaker is serving anything, see [Field diagnostics §3](#3-ap-is-up-but-every-tcp-port-is-refused--the-speaker-is-not-in-active-setup-mode) — "AP up but every TCP port refused = not in setup mode."

> **If resets don't help, the firmware itself is likely stuck** (a known issue, made worse by Bose shutting down the SoundTouch cloud on **6 May 2026**). Read [Root cause (2026): Bose cloud shutdown + stuck setup firmware](#root-cause-2026-bose-cloud-shutdown--stuck-setup-firmware) — the fix is an **offline USB firmware reflash**, and [SoundCork](https://github.com/timvw/soundcork/blob/main/docs/speaker-setup.md) can replace the dead Bose cloud afterward.

---

## Findings: reverse‑engineered Wave SoundTouch IV setup protocol

These findings were obtained by inspecting the **official Bose SoundTouch Android app** (`com.bose.soundtouch`, v27.0.2). The WiFi setup logic is implemented in JavaScript files (`workflow_gabbo_setup.js`, `gabbo_setup_bco.js`, `socket_comm.js`) embedded in the app's asset bundle.

**Key result: the Wave SoundTouch IV is the older “Gabbo / SM1” platform and does NOT use the SoundTouch‑10 telnet CLI on port 17000, nor the newer WebSocket path.** It runs a small embedded web server on its setup access point and is provisioned over plain HTTP.

### Two different SoundTouch setup families

| Platform | Example devices | Setup‑AP gateway | Provisioning mechanism |
|---|---|---|---|
| **SM1 / Gabbo** | **Wave SoundTouch IV (SoundTouch 4)**, Wave SoundTouch Music System IV, older Wireless Link adapter | **`192.168.1.1`** (port **80**) | HTTP form posts (this is what BosMan now uses) |
| **SM2 / dual** | SoundTouch 10/20/30, SoundTouch 300, newer SoundTouch‑SDK speakers | `192.0.2.1` (port **8080**) | WebSocket subprotocol `gabbo`, `addWirelessProfile` XML |
| ST10 telnet (legacy docs) | SoundTouch 10 (older firmware) | `192.0.2.1` (port **17000**) | Telnet CLI `network wifi profiles add …` |

The gateway map is hard‑coded in the official app: `{ SM1: "192.168.1.1", SM2: "192.0.2.1" }`.

### Wave IV (SM1 / Gabbo) HTTP setup protocol — what BosMan implements

Once the phone is joined to the speaker’s setup AP, the speaker is reachable at **`http://192.168.1.1`** (port 80, no TLS).

1. **Site survey (list nearby WiFi networks)**
   - `GET http://192.168.1.1/setup/index.asp`
   - The returned HTML embeds a JS string `networksRAW`.
   - Entries are joined by the literal delimiter **`zirgtspghwq`**.
   - Each entry is formatted `SSID (SECURITY CIPHER)` — e.g. `MyHome (WPA2 AES)`, `Cafe (WEP )`, `Open (None )`.
   - Networks whose SSID starts with `Bose ` are filtered out (the speaker’s own AP).
   - SSIDs may contain `(`, so the app splits on the **last** `(`.

2. **Provision (submit chosen WiFi)**
   - `POST http://192.168.1.1/goform/aformHandlerConfigureProfileSettings`
   - `Content-Type: application/x-www-form-urlencoded`
   - Fields: `ConfigManual`, `SSID`, `Passphrase`, `Key0`, `Security`, `Cipher`, `DHCPClient`, `IP`, `Mask`, `DefGW`, `DNSSrv1`, `DNSSrv2`, `ProxyServer`, `ProxyServerPort`.
   - `Security` values (uppercased): `NONE`, `WEP`, `WPAPSK`, `WPA2PSK`, `WPAWPA2PSK`. `Cipher`: `TKIP`, `CCMP`/`AES`, or `TKIP CCMP`.
   - WPA/WPA2 passwords go in `Passphrase`; WEP keys go in `Key0`.
   - Security/cipher are reconciled to mixed mode `WPAWPA2PSK` (`Cipher = "TKIP CCMP"`) when the scanned security and cipher don’t match a single mode — BosMan uses this for manual WPA entries as the universal choice.

3. After a successful POST the speaker leaves setup mode and joins the chosen network; the setup AP disappears. From then on BosMan controls it on the normal **port 8090** like any other SoundTouch speaker.

### What was fixed in BosMan

- Added a native `httpPost` to `WifiInfoPlugin.java` (bound to the local‑only WiFi link; the existing `tcpCommand` mangles CRLFs and can’t carry a real HTTP POST).
- Exposed `httpPost` / `nativeHttpPost` in `src/lib/native/wifiInfo.ts`.
- Rewrote `src/lib/bose/setup.ts` from the (non‑existent on Wave IV) telnet CLI on `192.0.2.1:17000` to the Gabbo/SM1 HTTP‑form protocol above.
- **Multi‑host gateway resolution.** Real units do **not** always match the SM1/SM2 subnet map (a Wave SoundTouch IV in the field handed out `192.0.2.x`, i.e. the *SM2* subnet, while still speaking the *SM1* HTTP‑form protocol on port 80). `setup.ts` now probes a candidate list — last‑known‑good host → live detected gateway → both well‑known Gabbo gateways (`192.168.1.1`, `192.0.2.1`) — and locks onto whichever actually serves `/setup/index.asp`. So the subnet no longer needs to be guessed.
- Updated the setup panel copy in `WiFiSetupPanel.svelte` / `+page.svelte`.

> **Subnet is not a reliable platform indicator.** Do not assume `192.0.2.x` ⇒ SM2/WebSocket. Probe the protocol (try `GET /setup/index.asp` on port 80 first); the Wave IV uses HTTP forms regardless of which subnet its DHCP server hands out.

> SM2/dual (`192.0.2.1:8080` WebSocket `addWirelessProfile`, XML `PerformWirelessSiteSurvey`) is **not** implemented — it only matters for first‑time onboarding of the *newer* SoundTouch models, not the Wave IV.

---

## Field diagnostics (hard‑won, read this before debugging)

These notes come from a live debugging session against a real **Bose Wave ST (90E9CA)** unit. They explain several traps that cost a lot of time.

### 1. Command‑line probes (`adb shell nc`, `ping`) LIE about reachability

On a setup AP there is **no internet**, so Android does **not** make that WiFi the default network. Any socket that is **not explicitly bound to the WiFi network** (e.g. `adb shell nc`, a plain `ping`) gets routed nowhere and is rejected by a kernel routing rule:

```
$ adb shell ip rule
14000: from all fwmark 0x0/0x20000 iif lo uidrange 1-10243 prohibit
```

`prohibit` returns **`EACCES` → "Permission denied"** on `connect()`. So:

- `nc 192.0.2.1 80` → `Permission denied` (looks like a firewall, but isn’t)
- `nc 192.0.2.4 1` (the phone’s **own** IP) → `Connection refused` (allowed — unbound loopback‑ish path)
- `ping -I wlan0 192.0.2.1` → **works** (the `-I` binds to the interface)

**BosMan is unaffected** because its native `WifiInfoPlugin` (`httpGet` / `httpPost` / `tcpPortScan`) calls `bindProcessToNetwork(wifiNetwork)` (process‑wide) / `retainLocalWifi()` before connecting. A bound socket gets the right fwmark and reaches `192.0.2.1`. **Always test through the app, not the shell.**

### 2. A VPN / kill‑switch is a *red herring* here

An active VPN (e.g. ExpressVPN with “Internet Kill Switch” / Network Lock) was initially blamed because killing it didn’t change the `Permission denied` result. **It was never the cause** — the unbound‑socket `prohibit` rule above produces the identical symptom. Disabling the VPN entirely (`pm disable-user`) did not change anything. Don’t rabbit‑hole on the VPN; verify with a *bound* request through the app first. (A VPN with lockdown *can* still interfere, but the WiFi binding handles the normal no‑internet case.)

### 3. "AP is up but every TCP port is refused" ⇒ the speaker is **not in active setup mode**

Symptom set on the broken unit, measured through the app’s own bound sockets:

- ICMP ping to `192.0.2.1` → **replies**
- DHCP → phone got `192.0.2.2`, ARP shows the gateway MAC `10:ce:a9:fd:79:be`
- `tcpPortScan` of `80,81,443,8080,8090,8091,8443,8888,17000,…` → **all refused (TCP RST in ~20 ms)**

ICMP/DHCP/ARP alive + **fast RST on every TCP port** = the speaker’s TCP stack is running but the **embedded setup web server is not**. The open SSID persists after setup mode times out, which is misleading. **Fix = put the unit back into setup mode** (see Factory reset / setup recovery below). A stale broadcasting AP alone proves nothing.

### 4. Verifying end‑to‑end through the installed debug app (CDP)

The debug APK’s WebView is debuggable, so you can drive BosMan’s real native networking from your dev machine without touching the UI:

```bash
adb forward tcp:9333 localabstract:webview_devtools_remote_$(adb shell pidof com.soundtouch.controller)
curl -s http://127.0.0.1:9333/json   # grab the ws:// page debugger URL
```

Then over that WebSocket, `Runtime.evaluate` against the page, e.g.:

```js
await window.Capacitor.Plugins.WifiInfo.getNetworkInfo();
await window.Capacitor.Plugins.WifiInfo.httpGet({ url: 'http://192.0.2.1/setup/index.asp', timeoutMs: 6000 });
await window.Capacitor.Plugins.WifiInfo.tcpPortScan({ host: '192.0.2.1', ports: '80,8090,8080', connectTimeoutMs: 1500 });
```

`getNetworkInfo()` returning `gateway: "192.0.2.1"` but `httpGet` failing with `Failed to connect to /192.0.2.1:80` (a clean `ConnectException`, **not** `Permission denied`) is the proof that the socket *is* bound to WiFi and the port is genuinely closed.

---

## Root cause (2026): Bose cloud shutdown + stuck setup firmware

A long live‑debugging session on a real **Bose Wave ST (90E9CA)** unit ended with this conclusion: **the failure is in the speaker firmware, not the app, the phone, or the network.** Two documented, dated issues explain everything we saw.

### 1. Bose shut down the SoundTouch cloud servers on **6 May 2026** (fleet‑wide)

Bose officially ended SoundTouch cloud support — provisioning, accounts, presets, TuneIn, and **the online firmware‑update servers** are gone.

- [Bose: SoundTouch end‑of‑life notice](https://www.bose.com/soundtouch-end-of-life)
- [Bose UK EOL landing page](https://www.bose.co.uk/en_gb/landing_pages/soundtouch-eol.html)
- [r/bose: shutdown date extended to 6 May 2026](https://www.reddit.com/r/bose/comments/1q6f3fc/bose_soundtouch_shutdown_update/)
- [Confirmed live on 6 May 2026](https://www.linkedin.com/posts/basroelofs_yesterday-i-posted-about-bose-corporation-activity-7458095003500834816-l-IN)

**Why this matters for setup:** un‑updated units phone home to Bose’s provisioning/update servers during first‑time setup. With those servers gone, the official app and any cloud‑dependent onboarding step can hang. **The Bose Updater website ([btu.bose.com](https://btu.bose.com)) and OTA updates can no longer be relied on** — use the offline USB method below.

### 2. The exact symptom we measured = a known “stuck firmware” bug

On the broken unit, through the app’s own bound sockets (see [Field diagnostics §3](#3-ap-is-up-but-every-tcp-port-is-refused--the-speaker-is-not-in-active-setup-mode)):

- the `Bose Wave ST (…)` AP **broadcasts** ✓
- **DHCP works** — the phone keeps getting fresh `192.0.2.x` leases ✓
- the speaker answers at L2/L3 — **ARP `REACHABLE`**, gateway MAC `10:ce:a9:fd:79:be` ✓
- **but every TCP port (80, 443, 8080, 8090, 8443, 17000, …) refuses instantly** ✗ — verified across 5 back‑to‑back probes.

**DHCP alive + web server dead is the signature of a half‑started / corrupted setup firmware** — the pedestal boots far enough to run dnsmasq but its embedded HTTP setup server never launches. This matches a documented case on the SoundTouch 20, where an interrupted firmware update left the unit showing an amber Wi‑Fi light, undiscoverable, with setup unreachable:

- [r/bose: “Big problem with SoundTouch 20 firmware” — fixed by USB reflash](https://www.reddit.com/r/bose/comments/pfz8sf/big_problem_with_bose_soundtouch_20_firmware_1/)

### 3. Amber light: “solid” vs “blinking” — what the official docs say

Bose’s “[Cannot connect to the built‑in setup network](https://support.bose.com/s/article/wstms-speakerwave-cannot-connect-to-the-built-in-setup-network-of-a-bose-product---ka08c000001pxgeaau)” article describes the alternate‑setup method as: **hold Control ~3 s and release the moment the amber indicator BLINKS.** Bose’s “[Putting a system into Setup mode](https://support.bose.com/s/article/wstmsiv-speakerwave-putting-a-system-into-setup-mode---ka08c000001pxf7aae)” article and our field unit show **SOLID amber + `SETUP SEE INSTRUCTIONS`** as the “server is up” state.

> On our faulty unit **neither** state produced a live port‑80 server, and we could not coax it into a *blinking‑amber* state at all — consistent with the stuck‑firmware diagnosis above, not a button‑sequence mistake. If your unit is healthy, treat **either** blinking‑then‑solid amber as “entering setup”; the real proof is port 80 answering, not the light color.

> **Field note:** Not being able to get the pedestal into a *blinking* amber state is expected when the firmware is stuck — the pedestal cannot even reach that boot stage. This is the stuck‑firmware signature. The realistic next step is the **offline USB reflash** (fix #1 below). Once the firmware is healthy, a FAT32 USB with the correct firmware file should restore the blinking‑amber setup state and bring port 80 back up. To track down the exact correct firmware file and USB layout for your specific Wave SoundTouch IV pedestal, start with the community reflash guide and archive linked below.

### Fixes, in order of effort

1. **Offline USB firmware reflash (the standard fix for a pedestal whose setup server won't launch).**
   - Full step-by-step guide including exact firmware files: **[README.flash.md](README.flash.md)**
   - Format a **≤32 GB USB stick as FAT32** (not exFAT). On macOS, strip the junk files or the speaker won't read it:
     ```sh
     mdutil -i off /Volumes/YOUR_USB
     rm -rf /Volumes/YOUR_USB/.fseventsd /Volumes/YOUR_USB/.Spotlight-V100
     rm -f  /Volumes/YOUR_USB/._*
     ```
   - Put the **correct‑generation** firmware file on it and insert into the **pedestal’s USB port**, then power‑cycle. Since Bose’s own updater/servers are down, use the community archive + guide:
   - [r/bose: SoundTouch firmware downgrade / offline reflash guide (archive + FAT32 steps)](https://www.reddit.com/r/bose/comments/1lb1uav/solved_bose_soundtouch_firmware_downgrade_guide/)

2. **Self‑host the dead Bose cloud with SoundCork** (so presets/streaming work again *after* the speaker is on your Wi‑Fi).
   - Project: <https://github.com/timvw/soundcork>
   - **Speaker setup guide (read this first): <https://github.com/timvw/soundcork/blob/main/docs/speaker-setup.md>**
   - Companion CLI: <https://github.com/timvw/bose>
   - Write‑up: [Keep your Bose SoundTouch alive after the shutdown](https://timvw.be/2026/02/17/keep-your-bose-soundtouch-speaker-alive-after-the-shutdown/)
   - Note: SoundCork enables **passwordless root SSH** via a USB `remote_services` flag file and redirects the speaker’s cloud URLs to your server. Port **8090** already needs no auth. Only do this on a trusted network.
   - **SSH is a separate USB pass** from firmware flash — `bose-usb-prep.sh --both` does not enable SSH during the flash. See [README.flash.md § Flash vs SSH](../bose-usb-flash/README.flash.md#flash-vs-ssh-two-separate-procedures).

3. **This app (BosMan)** controls the speaker **locally** over port 8090 / WebSocket 8080 and needs none of Bose’s cloud — but the speaker must first be on your Wi‑Fi, which requires the setup server (fix #1) to work at least once.

4. **If USB reflash fails → the pedestal module is faulty** and needs Bose service/replacement. The Wave radio itself is unaffected; it is specifically the SoundTouch Wi‑Fi pedestal.

---

## Factory reset / setup recovery (Wave SoundTouch series IV)

If the speaker won’t respond on its setup AP (section 3 above), put it firmly back into setup mode. Procedures below are from Bose’s official documentation.

> **Two different "resets" exist — don’t confuse them:**
> - The **SoundTouch pedestal** (the Wi‑Fi base) is reset/restarted with the **Control button on the back of the pedestal**. *This is the one that matters for Wi‑Fi setup.*
> - **`RESET ALL`** in the on‑screen setup menu is a **Wave system** factory reset (clock/presets/radio side) done from the **remote**. It does **not** fix the Wi‑Fi pedestal and is normally **not** what you want here.

1. **Re‑enter Setup mode (do this first)** —
   [Bose: Putting a system into Setup mode](https://support.bose.com/s/article/wstmsiv-speakerwave-putting-a-system-into-setup-mode---ka08c000001pxf7aae)
   - Press and **hold the Control button on the back of the pedestal for ~3 seconds**.
   - Release when the **Wi‑Fi light glows SOLID AMBER** and the display shows **`SETUP SEE INSTRUCTIONS`**.
   - That solid‑amber + `SETUP SEE INSTRUCTIONS` state is when the port‑80 setup server is actually running. (A merely broadcasting SSID is **not** enough — see section 3.)

2. **Restart the pedestal (soft — settings retained)** — official owner’s guide
   - Hold the pedestal **Control** button **> 10 seconds**, then release. After several seconds it turns on again and reconnects; the Wi‑Fi light returns to **solid white**. Then redo step 1.

3. **Hard‑reset the pedestal (clears Wi‑Fi config — forces fresh setup)** — Bose support / field‑confirmed
   - **Pull the AC power cord out of the wall outlet** (or the back of the unit) so the system goes **completely dark**.
     - ⚠️ "Unplug" = the **physical power cord**. Do **NOT** just press the power/standby button on the remote — standby leaves the pedestal powered and the reset will not take.
   - Press and **hold the Control button** on the back of the pedestal.
   - **While you are still holding Control, plug the power cord back into the wall.**
   - Keep holding Control for **~5 seconds**, then release. The system restarts with its Wi‑Fi configuration cleared.
   - Then redo step 1 to re‑enter setup mode.

4. **Wave *system* factory reset — `RESET ALL`** *(optional; resets clock/presets/radio, NOT the Wi‑Fi pedestal)* — from the **remote**:
   - Press and **hold the Alarm / Setup‑menu button** until **`-SETUP MENU-`** appears.
   - Press **Tune/MP3** (the Tune/MP3 control — two adjacent buttons used to scroll menu items) until **`RESET ALL- NO`** appears.
   - Press **Time + / Time –** to change the value to **`RESET ALL- YES`**; the display then shows **`PRESET 3 TO CONFIRM`**.
   - Press **Preset 3** to confirm → **`RESET COMPLETE`**.

5. **Disable / re‑enable Wi‑Fi** (occasionally clears a wedged radio)
   - Disable: hold the pedestal **Control** button **8–10 s** until the Wi‑Fi indicator turns **off**.
   - Re‑enable: power the system on with the remote.

> Note: the **Control button on the back of the pedestal** is the correct Wi‑Fi setup control; hold it until **solid amber + `SETUP SEE INSTRUCTIONS`**. Earlier drafts of this guide said "WiFi button → orange / blinking" and described `RESET ALL` as a front‑button confirm — both were wrong. The authoritative signals are **solid amber + display message** for setup mode, and **Preset 3** to confirm `RESET ALL` on the remote.

---

## Important: WiFi is not Bluetooth

| | WiFi (this guide) | Bluetooth |
|---|---|---|
| **What it does** | Network control, discovery, presets, zones, media servers | Pair a phone for audio streaming only |
| **Used by BosMan** | Yes — BosMan talks to the speaker over **Wi‑Fi** | **No** — BosMan does not use Bluetooth |
| **Button on base** | **WiFi** button (orange = setup mode) | **Bluetooth** button (separate) |
| **Typical use** | Control the speaker, see now playing, change volume | Play music from a phone to the speaker |

Pressing the **Bluetooth** button or pairing a phone over Bluetooth does **not** configure WiFi and does **not** help BosMan find or control the speaker. You need a working **WiFi** connection to the speaker (either its temporary setup network or your home router).

---

## What BosMan needs

BosMan is an independent app that controls SoundTouch speakers over the local network using the unofficial SoundTouch Web API:

- **HTTP** on port **8090** — commands, volume, presets, device info
- **WebSocket** on port **8080** — live updates (now playing, volume)

Your phone must be able to reach the speaker’s IP address on the network. That works in two ways:

1. **Direct setup WiFi** — phone joins the speaker’s own SSID (this guide’s main focus).
2. **Home WiFi** — phone and speaker are both on the same router (often easier long term; see [Alternative: home WiFi](#alternative-home-wifi) below).

---

## Part 1 — Put SoundTouch IV into WiFi setup mode (orange button)

These steps apply to the **Bose Wave SoundTouch Music System IV** with the **base / console** controller.

### 1. Prepare the system

- Plug in the Wave SoundTouch and turn it on.
- Use the **base controller** on the music system (not only the handheld remote).
- Move the speaker close to your phone during setup.

### 2. Enter WiFi setup mode

1. Locate the **Control button on the back of the pedestal** (the round multifunction button).
2. **Press and hold** it for **~3 seconds**, then release when **both**:
   - the **Wi‑Fi indicator glows SOLID AMBER**, and
   - the display shows **`SETUP SEE INSTRUCTIONS`**.
   This is the only state in which the speaker actually runs its setup web server (port 80). A broadcasting SSID **without** this state is not enough — see [Field diagnostics §3](#3-ap-is-up-but-every-tcp-port-is-refused--the-speaker-is-not-in-active-setup-mode).
3. If the light is white, green, off, or blinking but you never see `SETUP SEE INSTRUCTIONS`, the server may not be up — restart the pedestal or factory reset (see [Factory reset / setup recovery](#factory-reset--setup-recovery-wave-soundtouch-series-iv)).
4. Wait up to **one minute** for the setup network to appear.

### 3. Find the setup SSID on your phone

On the phone’s WiFi list, look for a network name similar to:

- `Bose SoundTouch Setup`
- `BOSE_SETUP_…`
- `SoundTouch…` with “Setup” or the speaker name

The exact name varies by model and firmware. There is **no internet** on this network — that is normal.

### 4. Confirm the speaker is in setup mode

You should have **all** of the following:

- WiFi button / indicator on the base is **orange**
- The Bose setup SSID is visible on the phone
- After connecting, the phone gets an address on the speaker’s subnet, commonly:
  - **`192.0.2.x`** (e.g. phone `192.0.2.7`) — seen on some Wave SoundTouch IV units
  - **`192.168.0.x`** (e.g. phone `192.168.0.111`) — older / other models

The speaker is usually at **`.1` on that subnet** (`192.0.2.1` or `192.168.0.1`), but not every phone can reach it if the OS drops the WiFi link too quickly (see Part 2).

### 5. Leave setup mode

Setup mode ends when:

- You finish configuring the speaker to join a home router (official Bose setup flow), or
- Setup times out after several minutes, or
- You press the WiFi button again / power-cycle the system (see Bose user guide)

**Important:** The **Wave SoundTouch IV (SoundTouch 4)** is the older **Gabbo / SM1** platform — it is **not** a SoundTouch 10. Its setup AP does **not** expose the ST10 telnet port `17000`; instead it runs an embedded web server at **`http://192.168.1.1`** (port 80) and is provisioned over HTTP (see [Findings](#findings-reverse-engineered-wave-soundtouch-iv-setup-protocol)). BosMan now speaks that HTTP setup protocol directly. Port **8090** (normal BosMan control) is **not** available on the setup AP — it appears only after the speaker joins your home WiFi.

---

## Part 2 — Configure your phone (GrapheneOS / Android)

Bose setup WiFi has **no internet**. Android and GrapheneOS often **disconnect after a few seconds** unless you change settings.

Do this **every time** you join the Bose setup network:

### A. When connecting

1. Turn **off mobile data** temporarily  
   **Settings → Network & internet → Mobile network → Mobile data → Off**
2. Connect to the **Bose setup SSID**.
3. When Android shows *“This network has no internet”*, tap **Stay connected** / **Use this network**.

### B. Per-network WiFi privacy (recommended)

1. **Settings → Network & internet → Internet**
2. Tap the **Bose** network → **Privacy** (or network details)
3. **MAC address type → Use device MAC**  
   (Do **not** use “Randomized MAC” for this network; some IoT APs drop randomized clients.)

### C. Optional on GrapheneOS

**Settings → Network & internet → Internet → ⋮ menu → Connectivity check → Off**

This reduces aggressive “no internet” disconnects on local-only networks.

### D. Automated setup via adb (USB debugging)

If the phone is connected over USB, run from the project directory:

```bash
chmod +x scripts/configure-grapheneos-bose-wifi.sh
./scripts/configure-grapheneos-bose-wifi.sh
```

Or for a different SSID:

```bash
BOSE_SSID="Bose SoundTouch Setup" ./scripts/configure-grapheneos-bose-wifi.sh
```

The script:

- Disables connectivity-check disconnects (`captive_portal_mode=0`, `wifi_avoid_bad_wifi=0`)
- Turns off mobile data temporarily
- Connects to the Bose SSID with **device MAC** (`-r none`)
- Opens **WiFi settings** so you can tap **Stay connected**

You still need to tap **Stay connected** once on the phone when Android warns *“This network has no internet”*. Without that, GrapheneOS may mark the network `NO_INTERNET_PERMANENT` and drop it after a few seconds.

When you open **BosMan**, approve the system dialog if Android asks whether BosMan may use the Bose WiFi network. BosMan uses `WifiNetworkSpecifier` to hold the local link while the app is open.

### E. VPN kill-switch — "Block connections without VPN" (field-confirmed blocker)

If you use a VPN on your Android phone (e.g. ExpressVPN), Android has a **system-level kill switch** that is separate from the VPN app's own Network Lock setting. When enabled it blocks **all** traffic — including local LAN addresses like `192.168.0.x` — whenever the VPN tunnel is down or when a connection would bypass the tunnel.

**Symptom:** Safari/Chrome on the phone shows *"Your internet access is blocked"* when opening `http://192.168.0.119:8090/info`, even though:
- the phone is on the correct subnet (`192.168.0.x`)
- ExpressVPN shows *"Not protected"* (tunnel is down)
- BosMan is listed in split tunneling

The VPN app's own "not protected" status and split-tunneling settings do **not** override this Android OS-level block.

**Fix:**

```
Android Settings
  → Network & internet
    → VPN
      → tap the gear icon next to ExpressVPN (or your VPN)
        → turn off "Block connections without VPN"
```

You can leave **Always-on VPN** enabled — that keeps the VPN reconnecting automatically. Only the block toggle needs to be off:

| Setting | Effect |
|---------|--------|
| Always-on VPN ON | VPN reconnects automatically if it drops — keep this |
| Block connections without VPN ON | Kills **all** traffic including LAN (192.168.x.x) when VPN is down — **turn this off** |
| Block connections without VPN OFF | If VPN drops, traffic falls back to normal routing; local devices always reachable |

After turning it off, `http://192.168.0.119:8090/info` in the phone browser and BosMan direct-IP connection both work immediately.

### F. Verify the link (USB debugging, optional)

With the phone on Bose WiFi and connected over USB:

```bash
adb shell ip -4 addr show wlan0
```

You should see something like `inet 192.168.0.xxx/24`. If the phone disconnects from Bose WiFi within seconds, fix Part 2 before using BosMan.

---

## Part 3 — Configure and use BosMan

### Install BosMan (Android)

From the project directory on a development machine:

```bash
npm run deploy:android
```

Or build manually:

```bash
npm run build:mobile
npx cap sync android
cd android && ./gradlew assembleDebug
adb install android/app/build/outputs/apk/debug/app-debug.apk
```

The launcher icon is **BosMan**.

### Use BosMan on Bose setup WiFi (replaces the Bose app)

1. Complete **Part 1** (orange WiFi on base) and **Part 2** (phone stays on Bose SSID).
2. Open **BosMan** while still connected to the Bose network.
3. BosMan detects the setup network and shows the **Set up home WiFi** panel.
4. Tap **Scan for home WiFi networks** (BosMan reads the speaker’s site survey from `http://192.168.1.1/setup/index.asp`), pick your router SSID, enter the password, and tap **Connect speaker to home WiFi**.
5. When the base WiFi light leaves orange (setup complete), connect the phone to the **same home WiFi** and tap **Search again** — BosMan will find the speaker on port **8090**.

On the setup AP, BosMan also:
   - Keeps using the local WiFi link (no internet required)
   - Talks HTTP to the Gabbo setup server at **`192.168.1.1`** (port 80)
   - Does **not** expect port **8090** until the speaker has joined home WiFi

### If no devices are found

1. Confirm the base WiFi light is still **orange**.
2. Confirm the phone did **not** switch back to LTE or home WiFi.
3. Tap **Search again** in BosMan.
4. Use **Connect by IP** and enter the Wave IV setup gateway:

   ```
   192.168.1.1
   ```
   On older / other models the setup gateway may instead be:
   ```
   192.0.2.1
   ```
   or
   ```
   192.168.0.1
   ```

5. If that fails, try power-cycling the speaker, re-enter setup mode (orange WiFi), and reconnect the phone.

### What works in BosMan

- Power, play / pause / stop
- Volume
- Presets
- Multi-room zones (if multiple speakers are available)
- Media server browsing (when servers exist on the same network)

### Desktop / laptop use

On a PC on the **same network** as the speaker:

```bash
npm install
npm run dev
```

Open `http://localhost:5173`. For a production server on your LAN:

```bash
npm run build
npm start
```

---

## Alternative: home WiFi

You do **not** have to use the speaker’s setup SSID for everyday use.

1. Put the speaker in setup mode (orange WiFi) once and configure it to join your **home router** (BosMan setup panel, which posts to the Gabbo HTTP setup server at `192.168.1.1`).
2. Connect the phone to the **same home WiFi**.
3. Open BosMan — it should discover the speaker via network scan or SSDP (on desktop).

This is often more stable than staying on the temporary Bose AP.

---

## Field-confirmed setup procedure (June 2026)

This section records exactly what worked against a live **Bose Wave SoundTouch IV** (`90E9CA`, firmware 27.0.6) that had already been reflashed via USB.

### What was removed (did not work)

- **Phone browser at `192.0.2.1`** — iOS/Android blocks the page with "your internet access is blocked" because the setup AP has no internet. The OS intercepts the request before it reaches the device.
- **`dns-sd -G v4 BoseSoundTouch-90E9CA.local`** — the `-G` lookup requires the exact mDNS hostname the device registered, which is **not** the same as the advertised instance name. This returned nothing.
- **`ping BoseSoundTouch-90E9CA.local`** — same reason; the hostname suffix was wrong.

### What worked — step by step

#### Step 1 — Connect a Mac (or Linux laptop) to the Bose setup SSID

macOS does not block page loads on no-internet networks the way iOS/Android does. Connect the Mac to the `Bose Wave ST (…)` SSID from System Settings → Wi-Fi. You may get a "no internet" notification — dismiss it and stay connected.

#### Step 2 — Open the setup page in a Mac browser

Navigate to:

```
http://192.0.2.1
```

This serves the **SoundTouch Access Point Setup** page — a WiFi credential form. Pick your home SSID, enter the password, and submit. The device leaves setup mode and joins home WiFi; the `Bose Wave ST` SSID disappears.

> The firmware update page is also reachable at `http://192.0.2.1:17008/update.html` — it shows the current firmware version ("From: 27.0.6") and lets you upload a `.stu` file manually.

> **Subnet note:** this unit handed out `192.0.2.x` (SM2 pattern) but the setup page is on port 80 (SM1/Gabbo HTTP form), not a WebSocket. BosMan's multi-host gateway probing handles this automatically.

#### Step 3 — Discover the device on home WiFi (Mac command line)

After the device joins home WiFi, find it with mDNS:

```bash
# 1. Browse for SoundTouch services — gives you the advertised instance name
dns-sd -B _soundtouch._tcp local
# Output example:
# 18:27:11.356  Add  ...  Bose SoundTouch 90E9CA

# 2. Resolve the instance name to host + port
dns-sd -L "Bose SoundTouch 90E9CA" _soundtouch._tcp local
# Output example:
# Bose SoundTouch 90E9CA._soundtouch._tcp.local. can be reached at
#   Bose-SM2-10cea9fd79bd.local.:8090 (interface 15)
# DESCRIPTION=SoundTouch MAC=7C010A90E9CA MANUFACTURER=Bose Corporation MODEL=SoundTouch

# 3. Resolve the mDNS hostname to an IP
ping Bose-SM2-10cea9fd79bd.local
# PING bose-sm2-10cea9fd79bd.local (192.168.0.119)
```

> Use the hostname from step 2 (`Bose-SM2-10cea9fd79bd.local` in this case) — **not** the instance name (`Bose SoundTouch 90E9CA`) — in `ping` or `dns-sd -G`. The instance name is a human-readable label; the hostname is the mDNS A-record.

#### Confirmed device identity

| Field | Value |
|-------|-------|
| mDNS instance name | `Bose SoundTouch 90E9CA` |
| mDNS hostname | `Bose-SM2-10cea9fd79bd.local` |
| MAC address | `7C:01:0A:90:E9:CA` |
| Home WiFi IP | `192.168.0.119` |
| Firmware | 27.0.6 |
| SoundTouch API | `http://192.168.0.119:8090` |

#### Step 4 — BosMan connects on port 8090

With the device on home WiFi, BosMan discovers it at `192.168.0.119:8090`. Port 8090 is **not** available on the setup AP — it only starts after the device joins home WiFi. "No SoundTouch API" errors on the setup AP are expected.

---

## Troubleshooting

| Symptom | Likely cause | What to do |
|--------|----------------|------------|
| Phone joins Bose WiFi then disconnects | Android “no internet” logic | Stay connected, disable mobile data, use device MAC (Part 2) |
| `No SoundTouch device at 192.168.0.1` | Wrong subnet — some units use `192.0.2.x` not `192.168.0.x` | BosMan now auto‑probes the gateway + both Gabbo subnets; if forcing, check phone IP (`adb shell ip -4 addr show wlan0`) and try `192.0.2.1` |
| `No SoundTouch API at 192.168.1.1` / setup panel finds nothing, but the AP is connected | Speaker **not in active setup mode** — AP broadcasting but the port‑80 server is down (every TCP port RST) | Re‑enter setup mode until **solid amber + `SETUP SEE INSTRUCTIONS`**; restart pedestal or `RESET ALL` ([recovery](#factory-reset--setup-recovery-wave-soundtouch-series-iv)). Verify with [Field diagnostics §3‑4](#field-diagnostics-hard-won-read-this-before-debugging) |
| `nc`/`ping` from `adb shell` says `Permission denied` to `192.0.2.1` | Unbound socket hits Android’s `prohibit` routing rule (no default network on an internet‑less AP) — **not** a VPN/firewall | Ignore the shell result; test through the app (it binds to WiFi). See [Field diagnostics §1‑2](#1-command-line-probes-adb-shell-nc-ping-lie-about-reachability) |
| “Search devices” finds nothing during setup | `Search` uses SSDP + port 8090, which a setup‑mode speaker doesn’t run yet | Use the **WiFi Setup panel** (not Search) to provision; Search only works after the speaker joins home WiFi |
| WiFi drops after ~5 seconds | `NO_INTERNET_PERMANENT` — Android rejected the network | Tap **Stay connected**, run `scripts/configure-grapheneos-bose-wifi.sh`, use device MAC |
| `No devices found on network` | Phone not on same LAN as speaker | Same SSID or same home router |
| BosMan works on PC but not phone | WebView / local HTTP on Android | Use latest BosMan APK (`CapacitorHttp` + WiFi retention) |
| Bluetooth works but BosMan does not | Bluetooth ≠ WiFi control | Use WiFi setup (this guide), not Bluetooth pairing |
| Browser / BosMan shows *"Your internet access is blocked"* when opening `192.168.0.x` on phone | Android OS kill-switch — **"Block connections without VPN"** is on (separate from the VPN app's own Network Lock) | **Settings → Network & internet → VPN → gear next to VPN → Block connections without VPN → Off**. Keep "Always-on VPN" on. See [Part 2 §E](#e-vpn-kill-switch----block-connections-without-vpn-field-confirmed-blocker) |

---

## Legal note

BosMan is **not** affiliated with Bose Corporation. “Bose”, “SoundTouch”, and related marks are trademarks of Bose Corporation. Use at your own risk.

For API details see `doc/Bose_SoundTouch_API_v1.1.0.md` and the main [README.md](README.md).