# Switch USB Flipper

A Flipper Zero app that makes the Flipper act as a wired Nintendo Switch Pro Controller over USB.
It answers the Switch's USB handshake, the setup/subcommand exchange (device info, SPI flash reads,
calibration) and the Bluetooth-pairing stages the Switch runs over USB, then streams input reports.

> **Unofficial.** Not affiliated with or endorsed by Nintendo. The app reports Nintendo's USB IDs
> (VID `057E` / PID `2009`) so the console treats it as a Pro Controller. That may go against Nintendo's
> terms of service. Use it at your own risk, and don't use it for macros, turbo or automation in online
> play, which is the likeliest way to put an account at risk.

## Build and install

```
pip install ufbt
ufbt                # builds dist/switch_pro.fap
ufbt launch         # builds, copies to the Flipper and runs it (Flipper connected by USB)
```

Or copy `dist/switch_pro.fap` to `apps/USB/` on the Flipper's SD card. Match the SDK to your firmware
(`ufbt update --channel=release` for stock, or the channel your custom firmware uses).

## Use

1. On the Switch: **System Settings > Controllers and Sensors > Pro Controller Wired Communication = ON**.
2. Run the app on the Flipper, then connect the Flipper's USB-C port to the dock's USB-A port
   (USB-C to USB-A data cable). While the app runs, the Flipper's USB serial/CLI is unavailable.
3. The status line goes `no host` > `waiting for Switch` > `handshake...` > `connected`.
4. To check it, open **Controllers and Sensors > Test Input Devices** on the Switch.

## Controls (6 keys, 5 layers)

Hold **OK** to cycle layers, hold **Back** to exit.

| Layer    | Up | Down | Left | Right | OK | Back |
|----------|----|------|------|-------|----|------|
| D-PAD    | D-pad Up | D-pad Down | D-pad Left | D-pad Right | A | B |
| FACE     | X | B | Y | A | R | L |
| TRIGGERS | ZR | ZL | L | R | A | B |
| L-STICK  | stick up | stick down | stick left | stick right | A | B |
| SYSTEM   | + | - | Home | Capture | A | B |

For basic menu use, the default D-PAD layer is all you need. Remap by editing the `layers[]` table in
`switch_pro_app.c`.

## Status

- Tested on a **Nintendo Switch (original)**: the console accepts the controller, completes the
  handshake and pairing, and sends rumble/setup traffic.
- Also recognized as a Pro Controller by **Steam** on a PC.
- Not tested on Switch 2, Switch Lite/OLED or other system versions. The pairing replies were worked out
  by trial against an original Switch, so other consoles or firmware versions may behave differently.
- Only the left stick is mapped; there is no IMU, rumble, NFC or battery reporting.

## Debug log

On exit the app writes `/ext/switch_pro_log.txt` to the SD card: the first 120 packets and USB events
(`R` = from host, `T` = to host, `E` = USB event; E codes: 0 deconfigure, 1 configure, 2 wakeup,
3 suspend). It helps when the console rejects the device. To turn it off, remove the `log_dump();` call in
`pro_usb_stop()` in `pro_usb.c`.

## How it works

`pro_usb.c` registers a custom USB HID interface with the Pro Controller's descriptors, replies to the
`0x80` USB commands and `0x01` subcommands, and sends `0x30` input reports about every 15 ms.
`switch_pro_app.c` is the UI and key mapping.

## License

GPL-3.0, see `LICENSE`.
