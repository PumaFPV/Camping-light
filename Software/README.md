# Touch lamp firmware

Bare-metal C for the ATtiny412 on the touch lamp board (schematic rev A). No Arduino core, no libraries beyond avr-libc. It builds to about 1.8 KB of the chip's 4 KB flash.

## Toolchain

On CachyOS (or any Arch-based system), everything comes from the official repos:

```sh
sudo pacman -S --needed avr-gcc avr-binutils avr-libc avrdude make
sudo usermod -aG uucp $USER    # access to /dev/ttyUSB*, then log out and back in
```

The ATtiny412 needs avr-gcc 8 or newer and avr-libc 2.1 or newer. Arch ships current versions. This code was build-tested with avr-gcc 15.2 and avr-libc 2.2.1, with no warnings under `-Wall -Wextra`. avrdude 7.0 or newer is required for the `serialupdi` programmer.

## Programmer

The tinyAVR 0/1-series is programmed over UPDI, a single wire on PA0. The cheapest programmer is a plain USB-serial adapter (CH340, CP2102 or FT232) turned into a SerialUPDI adapter:

| Adapter | Connection | Board (J2) |
|---|---|---|
| TX | 4.7 kΩ resistor to RX | |
| RX | direct | UPDI |
| GND | direct | GND |
| VCC | leave unconnected | |

If the adapter has a 3.3 V/5 V jumper, set it to 5 V. Power the board from its own battery or USB-C while flashing, never from the adapter. A ready-made alternative is Adafruit's UPDI Friend.

## Build and flash

```sh
make                          # builds lamp.hex and prints the size
make flash                    # uses /dev/ttyUSB0
make flash PORT=/dev/ttyUSB1  # if your adapter shows up elsewhere
```

If flashing is unreliable, lower the speed with `make flash BAUD=57600`.

## Fuses

The factory fuses are correct for this firmware, so you don't need to write any. `make fuses` prints them:

- **BODCFG (fuse1):** brown-out detection off. Low battery is handled in firmware, with the DW01A as a backup.
- **OSCCFG (fuse2):** 20 MHz oscillator. The firmware divides it by 2.
- **SYSCFG0 (fuse5):** PA0 works as UPDI.

Never change the RSTPINCFG bits in SYSCFG0. Turning PA0 into GPIO locks out UPDI, and recovery then needs a high-voltage programmer.

## Calibrating the voltage reading

The chip reads its own supply voltage against an internal 1.1 V reference. That reference is only accurate to a few percent, so the low-battery and cutoff thresholds shift by the same ratio. Every power-up stores the measured voltage in EEPROM so you can correct for it:

1. With the battery in and USB-C unplugged, flash the firmware and wait for the short blink.
2. Measure the battery with a multimeter.
3. Run `make vdd`. It prints two bytes, low byte first. For example, `0x74,0xe` means 0x0E74, which is 3700 mV.
4. Set `VREF_MV = 1100 × measured / reported` in `main.c`, then rebuild and flash.

## Settings

All the settings are at the top of `main.c`.

| Setting | Default | What it does |
|---|---|---|
| `AUTO_OFF_MIN` | 240 | Fades off after this many minutes. Set to 0 to disable. |
| `LEVEL_DEFAULT` | 140 | Brightness after the battery is connected (1–255). |
| `T_HOLD_ON` | 350 ms | How long you hold to switch on. Raise it if rain still triggers the lamp. |
| `V_LED_MV` | 2850 | LED forward voltage. Measure it across one LED with the lamp on full. |
| `V_NOM_MV` | 3700 | Brightness is held at what it would be at this cell voltage. Lower keeps brightness constant deeper into the discharge; higher allows more light on a full cell. |
| `V_LOW_MV` / `V_CUTOFF_MV` | 3450 / 3300 | Low-battery warning, and minimum to switch on / fade off. |

## How it behaves

- **Switching on:** hold about 0.35 s. Short taps are ignored, so rain doesn't switch it on.
- **When on:** tap to switch off. Hold to dim or brighten; each hold reverses direction.
- **Brightness memory:** the level is remembered as long as the battery stays connected.
- **Water on the pad:** a touch held longer than 12 s is treated as water. It's ignored, and the chip goes back to sleep until the pad clears.
- **Low battery:** two dips as a warning, then a slow fade off. Three short blinks mean the battery is too low to switch on.
- **USB with the lamp off:** two slow pulses mean charging, one long glow means already full. You get another long glow when charging finishes.
- **Asleep (off and unplugged):** the ATtiny sits in power-down at about 0.1 µA, woken only by the touch pad or by plugging in USB.
