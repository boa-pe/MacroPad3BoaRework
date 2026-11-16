# MacroPad 3 – User manual

MacroPad 3 is a small programmable keyboard: 9 keys and a rotary knob (the *encoder*) that you can press as well as turn. Every key, both turning directions and the knob press can be given their own function. These are stored in 9 *profiles*, which you can switch between at any time.

You set everything up from a web page in your browser, served by the macropad itself. Nothing needs to be installed on your computer.

---

## Contents

1. [At a glance](#1-at-a-glance)
2. [Connecting to a computer](#2-connecting-to-a-computer)
3. [The status light](#3-the-status-light)
4. [Profiles](#4-profiles)
5. [First-time setup (Wi-Fi)](#5-first-time-setup-wi-fi)
6. [Opening the settings page](#6-opening-the-settings-page)
7. [Changing what a key does](#7-changing-what-a-key-does)
8. [Device settings](#8-device-settings)
9. [Wi-Fi networks](#9-wi-fi-networks)
10. [Status and maintenance](#10-status-and-maintenance)
11. [Troubleshooting](#11-troubleshooting)

---

## 1. At a glance

```
  ┌───────┐   ┌───┬───┬───┐
  │   ↺   │   │ 1 │ 2 │ 3 │
  │ (●)   │   ├───┼───┼───┤
  │   ↻   │   │ 4 │ 5 │ 6 │
  └───────┘   ├───┼───┼───┤
  encoder     │ 7 │ 8 │ 9 │
              └───┴───┴───┘
```

| Part | Normal use | Special use |
|---|---|---|
| Keys 1–9 | Whatever you mapped them to | Hold **key 1** while plugging in: Wi-Fi setup mode |
| Turn the knob left / right | Two separate functions, sent once per click of the knob | In profile selection: previous / next profile |
| Short press of the knob | Its own function (sent when you let go) | In profile selection: confirm |
| Long press of the knob (1.5 s) | Enter profile selection | In profile selection: cancel |

Out of the box:

- **Profile 1 "Numpad"** (green): keys 1–9 type numpad 1–9. Turning the knob changes the volume, and pressing it mutes.
- **Profiles 2–9** are empty, ready for your own functions. Their colours: 2 red, 3 blue, 4 yellow, 5 magenta, 6 cyan, 7 orange, 8 purple, 9 white.

> Numpad keys type digits only while **Num Lock** is on.

---

## 2. Connecting to a computer

### USB

Plug the macropad into the computer with a USB-C data cable. It works straight away as a keyboard; no driver is needed on Windows, macOS or Linux.

While the macropad is connected to a computer by USB, **Bluetooth is switched off**. Keys go over the cable only.

### Bluetooth

When there's no computer on the USB port (for example, it's on a phone charger or a power bank), the macropad becomes a Bluetooth keyboard.

1. Open your computer's or phone's Bluetooth settings and add a new device.
2. Choose **MacroPad 3** (you can rename it, see [Device settings](#8-device-settings)).
3. Pairing completes without a PIN.

Next time it reconnects by itself. The **Bluetooth** label in the settings page header shows the connection and an estimated delay.

> Bluetooth adds a few milliseconds of delay; the computer decides how often it talks to the keyboard. For gaming or anything timing-critical, use USB.

---

## 3. The status light

The light always shows the **colour of the active profile**. Its pattern tells you what the macropad is doing:

| Pattern | Meaning |
|---|---|
| Steady | Connected to a computer (USB or Bluetooth) |
| Slow "breathing" | No computer connected, waiting for USB or Bluetooth |
| Fast blinking (4× per second) | **Profile selection.** The colour is the profile you are about to choose |
| Slow blinking (once a second) | **Wi-Fi setup mode** (its own access point is on) |
| White double flash | The settings page is waiting for you to **press the code keys** |
| Fast white flashing | A firmware update is being installed. Don't unplug |

---

## 4. Profiles

A profile is a complete set of key functions, plus its own light colour and scan rate. There are 9.

### Switching profiles on the macropad

1. **Hold the knob down for 1.5 seconds.** The light starts blinking fast: you are in profile selection.
2. Choose a profile:
   - **turn the knob** to step through the profiles; the light blinks the colour of each one. Nothing changes yet.
   - **short-press the knob** to make the profile shown active, or
   - **press key 1–9** to jump straight to that profile and make it active.
3. To leave **without** changing anything, **hold the knob for 1.5 seconds** again. The previous profile stays active.

The key you press to choose a profile is never sent to the computer.

### Switching profiles in the settings page

Click a profile's tab, then **Make active**. The active profile is marked *● active*.

---

## 5. First-time setup (Wi-Fi)

The settings page needs the macropad on your Wi-Fi network. The first time, you tell it which network to use:

1. Plug the macropad in. With no Wi-Fi saved, the light **blinks slowly**: it has opened its own Wi-Fi network called **MacroPad-XXXX**.
2. On your phone or laptop, connect to **MacroPad-XXXX** (no password). The settings page usually opens by itself. If not, open **http://192.168.4.1** in a browser.
3. Press the 4 keys the page shows (see [Opening the settings page](#6-opening-the-settings-page)).
4. Under **Wi-Fi networks**, press **Scan**, choose your network, type its password and press **Save networks**.
5. The macropad restarts and joins your network. Reconnect your phone or laptop to your normal Wi-Fi.

To get back into setup mode later, **hold key 1 while plugging the macropad in**.

---

## 6. Opening the settings page

On a computer or phone on the same network, open:

> **http://macropad.local**

If that doesn't load (some phones and older systems don't support `.local` names), use the macropad's IP address instead. It's shown in your router's device list, and in the settings page under *Status & maintenance*.

### The code check

Before showing anything, the page asks you to prove the macropad is in front of you:

1. The page shows a picture of the 3×3 keys, with **4 keys numbered 1 to 4**.
2. Press those keys on the macropad, **in that order**. The light flashes white twice a second while it waits, and the presses are not sent to your computer.
3. Each correct key turns green. After the fourth, the settings page opens.

The code is new each time and is valid for 60 seconds. A wrong key ends the attempt; click **Show a new code** to try again. Pressing the knob cancels the check.

You stay signed in until:
- **10 minutes pass without you using the page** (just leaving it open doesn't count as use),
- you click **Lock** in the top-right corner, or
- the macropad restarts.

---

## 7. Changing what a key does

In the **Profiles** section:

1. Click the profile's tab (1–9) at the top.
2. Click the input to change:
   - a key in the **3×3 grid**, or
   - a part of the **knob**: the left half (↺ turn left), the right half (↻ turn right) or the middle (**PRESS**).

   The selected input gets a green edge. Each input shows its current function. While the page is open, keys you press on the macropad light up, and the knob halves flash as you turn it.
3. Below the grid, choose the **action** type and set it up (see below).
4. Click **Save profile**. Nothing is stored on the macropad until you do.

**Revert** throws away unsaved changes to this profile. **Reset to default** restores the factory setup for this profile.

You can also change the profile's **Name**, **Colour** (also the light colour) and **Scan rate** (see [Scan rate](#scan-rate)).

### Action: Key / shortcut

A keyboard picture shows the current key (or shortcut) highlighted in green.

- **Click a key** to choose it. Click it again to clear it.
- **Click modifier keys** (Ctrl, Shift, Alt, Win, AltGr, and the right-hand versions) to add or remove them. You can combine several, e.g. Ctrl + Shift + V.
- **Or type it:** click the keyboard picture, then press the shortcut on your own keyboard.
- **Power, Sleep, Wake** (top right of the picture) turn the computer off, put it to sleep, or wake it. These keys always work alone, without modifiers.

As soon as your choice differs from the current one, **✓** and **✗** appear in the top-right corner of the picture:
- **✓** accepts the new key (the input's label updates).
- **✗** (or **Esc**) goes back to the current one.

The key stays pressed for as long as you hold the macropad key, just like a normal keyboard, so the computer repeats it when held.

### Action: Media key

A panel of media buttons is shown, grouped as:

| Group | Functions |
|---|---|
| Playback | Previous track, Play / pause, Next track, Stop |
| Volume | Volume down, Mute, Volume up |
| Display | Brightness down, Brightness up |
| Apps | Calculator, File explorer, Mail, Search |
| Browser | Back, Forward, Refresh, Home |

Click one, then **✓** to accept or **✗** to go back.

### Action: Type text

Type up to 47 characters. Pressing the key types the whole text once. Write `\n` for Enter.

> Text is typed as if on a **US keyboard layout**. If your computer uses another layout (e.g. Polish, German), letters and digits are fine, but some symbols may come out differently.

### Action: None

The key does nothing.

### Knob turns

Each click of the knob sends its function once, as a short tap. Volume up/down is the classic use; arrow keys, Page Up/Down or zoom shortcuts (Ctrl + = / Ctrl + -) also work well.

### Scan rate

How often the macropad checks its keys: 1, 2, 4, 8 or 16 kHz (default 4 kHz). Higher means a key press is noticed sooner. Over USB, the computer collects keys once per millisecond however fast the scan is. A higher scan rate still means a press is more often ready for the next collection. For normal use the default is plenty.

---

## 8. Device settings

In the **Device** section:

| Setting | What it does |
|---|---|
| **Debounce (ms)** | How long a key is ignored after a change, to filter out contact bounce. Default 5 ms. Increase it if a single press sometimes registers twice. |
| **LED brightness** | Brightness of the status light (changes immediately). |
| **Encoder counts / detent** | Match this to your knob: if one click of the knob counts as 2 or 4 steps, or needs 2 clicks for one step, change this (usually 4). |
| **Reverse encoder** | Swap left and right turning. |
| **Bluetooth name** | The name computers show when pairing. |
| **Host name** | The `.local` name of the settings page (applies after a restart). |
| **Wi-Fi enabled** | Turn off Wi-Fi completely. This leaves the radio to Bluetooth alone; to reach the settings page again, hold key 1 while plugging in. |

Click **Save device settings**.

---

## 9. Wi-Fi networks

The macropad can remember **up to 5** Wi-Fi networks, for example home, office and a phone hotspot. It joins the strongest one in range and tries the others if that fails. The network it is on is marked *● connected*.

- **Add a network:** press **Scan**, then choose it from *Add from scan*, or use **Add manually** for a hidden network. Enter the password (leave it empty for an open network).
- **Remove a network:** click the **✗** next to it.
- **Change a password:** type the new password into its field. Leave the field empty to keep the saved password; saved passwords are never shown.

Click **Save networks**. Changes apply straight away, without restarting.

---

## 10. Status and maintenance

The **Status & maintenance** section shows:

| Item | Meaning |
|---|---|
| Firmware | Installed firmware version |
| Host link | USB, BLE (Bluetooth) or NONE |
| Latency | Estimated delay from pressing a key to the computer receiving it (see below) |
| Scan rate | Current key scan rate |
| Encoder detents | Knob clicks counted left / right since start |
| IP | The macropad's address on your network |
| Uptime, Free heap | Time since start, free memory |

**About the latency figure:** the macropad measures its own part on every key press: the time until the computer has collected the key over USB, or until the key is ready to send over Bluetooth. It then adds the parts it can calculate but not measure:
- the scan interval, and
- for Bluetooth, the computer's connection interval.

The top bar shows the average (e.g. *USB ≈ 0.70 ms*); hover over it for the worst case. Time spent inside the computer after the key arrives isn't included.

Buttons:

| Button | What it does |
|---|---|
| **Export config** | Downloads all profiles and settings as a `.json` file (Wi-Fi passwords are not included). |
| **Import config** | Loads a previously exported file (your Wi-Fi networks are left as they are). |
| **Forget Bluetooth pairings** | Removes every paired computer. Pair them again afterwards. |
| **Reboot** | Restarts the macropad. |
| **Factory reset** | Erases all profiles, settings, Wi-Fi networks and pairings. The macropad restarts in Wi-Fi setup mode. |
| **Firmware update** | Choose a firmware `.bin` file and click **Upload**. The light flashes white during the update; don't unplug. The macropad restarts when done. |

---

## 11. Troubleshooting

**The settings page doesn't open at `http://macropad.local`.**
Use the IP address instead (from your router's device list). Make sure your phone or computer is on the same Wi-Fi network as the macropad. If the light breathes or is steady but the macropad isn't on Wi-Fi, it may be out of range of all saved networks. Hold key 1 while plugging in to enter setup mode and check the network list.

**The page says "The macropad is not reachable".**
The macropad restarted or left the network. Reload the page after a few seconds.

**I pressed the code keys, but nothing happened.**
Check the picture: key 1 is top-left and key 9 bottom-right. The light should be flashing white; if it isn't, the code has expired, so click **Show a new code**.

**The keyboard stopped typing for a moment.**
While someone has the code check open, key presses go to the check (the light flashes white). Pressing the knob cancels it, and it also ends by itself after 60 seconds.

**A key registers twice.**
Increase **Debounce** in *Device*, e.g. to 8–10 ms.

**The knob skips or needs two clicks per step.**
Change **Encoder counts / detent** in *Device* (try 4, then 2). If it turns the wrong way, tick **Reverse encoder**.

**Bluetooth doesn't connect or the power keys don't work over Bluetooth.**
Remove *MacroPad 3* from the computer's Bluetooth settings, click **Forget Bluetooth pairings** in the settings page, and pair again. This is needed after firmware updates that change the Bluetooth keyboard layout.

**Bluetooth is "off" although I want to use it.**
Bluetooth is off whenever a computer is connected by USB. Unplug it from the computer and power it from a charger or power bank instead.

**Typed text comes out with wrong symbols.**
Text is typed for a US keyboard layout; switch the computer's layout to English (US), or use only letters and digits.

**Numpad keys move the cursor instead of typing digits.**
Turn on **Num Lock** on the computer.

**The computer doesn't wake up with the Wake key.**
Over USB, the computer must allow the device to wake it. On Windows: Device Manager → the macropad's keyboard entry → Power Management → *Allow this device to wake the computer*. Over Bluetooth, a sleeping computer is usually disconnected and can't be woken.

**I forgot everything / want to start over.**
Use **Factory reset** in the settings page. If you can't reach the page, hold key 1 while plugging in to get into Wi-Fi setup mode, open `http://192.168.4.1` and reset from there.
