# Neko M550D Firmware

Arduino Uno firmware for the Neko M550D multimeter (v1.2.7).

## Build / upload

The folder and `.ino` file names must match.

```bash
arduino-cli compile --fqbn arduino:avr:uno .
arduino-cli upload -p COM9 --fqbn arduino:avr:uno .
```

Libraries used:

- `Wire`
- `LiquidCrystal_PCF8574`

## Notes

- VCC is calibrated at every boot using the TL431 2.5 V reference.
- The voltage/diode probe input is selected by relay R5 on D10: NC = voltage, NO = diode.
- Source-code comments are in Turkish.
- The separate calibration helper is in `../kalibrasyon/`.
