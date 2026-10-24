# Neko M550D — Serial Protocol

- **Protocol version:** P1
- **Firmware:** v1.2.7
- **Baud:** 115200, 8N1, ASCII
- **Line ending:** the device sends `\n`; the PC may send `\n` or `\r\n`
- **No checksum** (USB serial connection assumed)

## Connection and startup

- Opening the COM port asserts DTR and **resets the Arduino**. The first line arrives after about **3–4 seconds** (splash, relay test, calibration).
- The PC application should wait for the first line (5 s timeout is a good default). Do not send commands before it.
- The status stream normally runs every **250 ms**. It pauses during boot and calibration; clients must tolerate gaps.
- Buffer incoming data by line; partial lines may arrive.

## Device → PC

Approximately every 250 ms, exactly **4 fields**, comma-separated:

```
MODE,VALUE,BATTERY,HOLD\n
```

| Field | Description |
|---|---|
| MODE | `M`=menu, `V`=voltage, `I`=current, `D`=diode, `C`=charging screen, `W`=charging warning |
| VALUE | `V`: voltage, 2 decimals (V) · `I`: current, 3 decimals (A) · `D`: Vf, 3 decimals (V) · `M/C/W`: `-` · open circuit / out of range: `OL` |
| BATTERY | Battery percentage, 0–100 |
| HOLD | `0` or `1` |

Examples:

```
M,-,85,0
V,12.34,85,0
I,1.234,84,0
D,0.652,84,1
V,OL,83,0
```

Notes:

- `BATTERY` may be `0` until the first battery sample after boot (about 1 s).
- For the first ~50 ms after entering a measurement mode, the value may briefly be `0.00` or the previous value. This is normal.
- While HOLD is active, the measurement value is frozen; the status stream continues with `HOLD=1`.
- HOLD is cleared automatically when leaving a measurement mode.
- Charge detection is based on a battery-percentage increase, so transitions to `C`/`W` are not necessarily instant.

## PC → Arduino

- Only the **first character** of a line is interpreted; commands are case-insensitive.
- The first character must be the command (no leading whitespace).
- A line ending (`\n` or `\r\n`) is required before a command is processed.
- Lines longer than 7 characters are truncated; the first character is still used.
- Unknown commands are **silently ignored**.

| Command | Action | Response |
|---|---|---|
| `V` | Switch to voltage mode | Status line |
| `I` | Switch to current mode | Status line |
| `D` | Switch to diode mode | Status line |
| `M` | Return to menu | Status line |
| `H` | Toggle HOLD (only in `V`/`I`/`D`) | Status line |
| `?` | Request current status | Status line |
| `#` | Device/protocol identity | `#NekoM550D,P1,FW1.2.7\n` |

**Response behavior:**

- Every recognized command returns a line immediately (status line or `#` identity line).
- A rejected `V`/`I`/`D` command still returns the **current** status line; the mode does not change.
- Sending the current mode again does not restart it; a status line is returned.
- Direct switching between `V`/`I`/`D` is allowed.

### Safety rules

- While a charging screen is active (`C`/`W`), `V`/`I`/`D` are ignored; `M` and `H` have no effect either. A status line is still returned.
- If the battery is critical (`≤ 8%`), entering a measurement mode is rejected (with an audible warning).
- If the battery becomes critical during measurement, the device **returns to the menu automatically**; the PC will see an unsolicited `M,...` line. This is normal.
- If the charger is connected during measurement, the device switches to `W`; measurement stays unavailable until the charger is removed.

## Example session

```
(port opened; waiting for the first line)
M,-,63,0
> #
#NekoM550D,P1,FW1.2.7
> V
V,0.00,63,0
V,12.34,63,0
> H
V,12.34,63,1
> M
M,-,63,0
```

## Client checklist

1. Open the port at 115200 8N1; wait for the first line (5 s).
2. Buffer lines, split on commas, and verify **4 fields**.
3. Treat `OL` and `-` as valid values.
4. Send `#` and verify `P1` and the firmware version.
5. Allow at least 1 s for command responses; if none arrives, refresh the state from the periodic stream.
6. Always take the mode from incoming lines; do not assume the mode after sending a command.
