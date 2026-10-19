# Retro Console + MP3 Player (ESP32)

A handheld retro game console with seven arcade-style games and a built-in MP3 player that shows album covers, all running on an ESP32 DevKit with a 1.8" ST7735 colour display.

## Features

- **7 games:** Snake, Tetris, Breakout (with power-ups), Flappy, Dino Run, Pong, Car Rush
- **MP3 player** reading tracks from a microSD card
- **Album cover art and song info** read from the MP3's ID3 tags (JPEG covers)
- **Sound effects** from a passive buzzer, with an on/off setting
- **High scores** saved to flash memory, one per game
- **Joystick + 4 buttons** controls
- **Settings screen** with a joystick test and crash-recovery reset
- **Splash screen** and a crash guard that reports the last reset reason

## Hardware

| Part | Notes |
|---|---|
| ESP32 DevKit V1 | Select **ESP32 Dev Module** in the Arduino IDE (not ESP32-S2) |
| 1.8" ST7735 TFT, 128x160 | Colour, landscape use |
| Analog joystick | Two analog axes (VRX, VRY), SW not used |
| 4 push buttons | A, B, START, SELECT |
| Passive buzzer | Sound effects |
| MAX98357A I2S amplifier | Drives the speaker for MP3 playback |
| Speaker | 4 to 8 Ω, about 3 W or more |
| microSD card module | Any standard SPI microSD module |
| microSD card | FAT32, 32 GB or smaller |
| Capacitors | 470 µF to 1000 µF (10 V or 16 V) at the SD module and the amp, plus 100 nF ceramics |
| Power | 5V supply rated 1 A or more recommended. A USB port works for light use |

## Wiring

### TFT display (ST7735)

| TFT pin | ESP32 GPIO |
|---|---|
| CS | 5 |
| DC | 16 |
| RST | 17 |
| SCK | 18 |
| SDA (MOSI) | 23 |
| LED (backlight) | 3V3 |
| VCC | 3V3 |
| GND | GND |

### microSD module (uses its own SPI bus, HSPI)

| SD module pin | ESP32 GPIO |
|---|---|
| VCC | 3V3 or 5V (check your module's label) |
| GND | GND |
| MISO | 26 |
| MOSI | 25 |
| SCK | 27 |
| CS | 13 |

Put a 470 µF to 1000 µF capacitor across the module's VCC and GND, and a 100 nF ceramic beside it. This stops the card's power spikes from resetting the board.

### MAX98357A I2S amplifier

| Amp pin | ESP32 GPIO |
|---|---|
| BCLK | 21 |
| LRC (WS) | 22 |
| DIN | 4 |
| VIN | 5V |
| GND | GND |
| SD (shutdown) | **3V3** (must be high, or the amp stays silent) |
| GAIN | Leave unconnected |

The speaker connects to the amp's **+** and **−** outputs, not to the ESP32.

### Buzzer

| Buzzer | ESP32 |
|---|---|
| + | GPIO 2 |
| − | GND |

### Joystick

| Joystick | ESP32 |
|---|---|
| VRX | GPIO 34 |
| VRY | GPIO 35 |
| VCC | 3V3 |
| GND | GND |

### Buttons

Each button connects one leg to its GPIO and the other leg to GND. The inputs use internal pull-ups.

| Button | ESP32 |
|---|---|
| A | GPIO 32 |
| B | GPIO 33 |
| START | GPIO 14 |
| SELECT | GPIO 15 |

## Software setup

### 1. Arduino IDE

Install the **Arduino IDE** and add the ESP32 board support package through the Boards Manager. Select:

- **Board:** ESP32 Dev Module
- **Flash size:** 4MB
- **CPU frequency:** 240MHz
- **Upload speed:** 921600

### 2. Libraries

Install these from the Library Manager:

- Adafruit GFX Library
- Adafruit ST7735 and ST7789 Library
- Adafruit BusIO
- **ESP8266Audio** (MP3 decoding)
- **TJpg_Decoder** by Bodmer (album cover decoding)

SPI, SD, and Preferences come with the ESP32 core.

### 3. Upload

Open `retro_console_v5.ino` and click Upload. Open the Serial Monitor at **115200 baud** to see boot messages, such as `SD: OK` and the number of MP3 files found.

### 4. SD card

- Format the card as **FAT32** (32 GB or smaller). Cards above 32 GB are usually exFAT, which the ESP32 cannot read.
- Copy your `.mp3` files to the **root** of the card (not into folders).
- Use **standard MP3 files** (128 kbps is the safest). Unusual or damaged files may stall playback. The player skips them automatically.

## Controls

| Input | Menu | Games | MP3 player |
|---|---|---|---|
| Joystick left/right | Choose game | Move (game-specific) | Previous / next track |
| Joystick up/down | - | Move (game-specific) | Volume up / down |
| A | Open the selected item | Action (jump, rotate, launch, etc.) | Play / pause |
| B | - | Action (hard drop, etc.) | - |
| START | - | Pause menu | - |
| SELECT (hold 0.7 s) | Switches between games and MP3 player | Switches to MP3 player | Switches back to games |

Joystick left/right and up/down also move the cursor in menus and settings.

### Game controls

- **Snake:** Joystick to steer. The snake wraps around the screen edges.
- **Tetris:** Left/right to move, A to rotate, down to soft-drop, B to hard-drop. A ghost piece shows where it will land.
- **Breakout:** Left/right to move the paddle, A to launch the ball. Catch the falling capsules: **M** = multi-ball, **W** = wide paddle, **S** = shield, **L** = slow ball, **+** = extra life.
- **Flappy:** A or up to flap.
- **Dino Run:** A or up to jump, down to duck.
- **Pong:** Up/down to move your paddle. First to 7 points wins.
- **Car Rush:** Left/right to change lanes. Hold A for nitro. Collect coins and avoid oncoming traffic.

## Menu items

Games: **SNAKE, TETRIS, BREAKOUT, FLAPPY, DINO RUN, PONG, CAR RUSH**, then **MUSIC** and **SETTINGS**.

Each game shows its high score. Scores are saved to flash memory.

## MP3 player

- Shows the **song title** and **artist** from the ID3 tags (or the filename if there are no tags).
- Shows the **album cover** on the left if the MP3 contains a JPEG cover.
- Shows **elapsed and total time** and a progress bar.
- Auto-plays the next track when a song ends.
- Left once in a song (more than 3 seconds in) restarts it. Left in the first 3 seconds goes to the previous track.
- Music pauses when you start a game and when you leave the MP3 player. Press A to play again.

## Settings

- **Sound FX:** turn buzzer sounds on or off.
- **Music volume:** adjust the MP3 volume.
- **Joystick test:** shows the raw joystick values and the direction read. Use it to check the joystick mapping.
- **Reset safe mode:** clears the crash-recovery counter if the console disabled a feature after repeated crashes.

Press B to save and return to the menu.

## Crash recovery

If the console resets repeatedly, it turns off the album covers, then the SD card, to keep the games working. The splash screen shows `LAST RESET` with the reason. Use **Settings → Reset safe mode** after fixing the problem.

## Troubleshooting

| Problem | Likely cause and fix |
|---|---|
| `SD: FAILED` in serial | Check SD wiring and that the card is FAT32 and 32 GB or smaller. Try a shorter wire or a larger capacitor on the module. |
| Screen goes blank or glitches when the SD card is inserted | Power dip from the card. Add 470 µF to 1000 µF across the SD module's power and ground. |
| Song plays on screen but no sound | Check the amp's **SD pin is at 3V3**, VIN at 5V, and the speaker on the amp's +/- outputs. Run the I2S tone test to isolate the amp. |
| Album cover missing | The track has no ID3 cover, the cover is PNG (only JPEG is supported), or it's larger than 80 KB. |
| Song title shows the filename | The MP3 has no ID3 title tag. |
| Watchdog or brownout reset | Usually power. Use a stronger 5V supply and extra capacitors. |
| Joystick directions are wrong | Use the **Joystick test** in Settings. Flip `JOY_INVERT_X`, `JOY_INVERT_Y`, or `JOY_SWAP_XY` in the sketch. |
| Serial Monitor shows garbage | Set the monitor to **115200 baud**. Use a data-capable USB cable. |

## Known limitations

- Album covers must be **JPEG** and under about **80 KB**. PNG covers are not supported.
- Only ID3v2.3 and v2.4 tags are read. Very old ID3v2.2 tags are ignored.
- Only **FAT32** cards up to 32 GB work.
- The ESP32 has limited RAM, so very large MP3s or unusual files may not play smoothly.

## Files

- `retro_console_v5.ino`: main sketch
- `i2s_tone_test.ino`: standalone test to check the amplifier and speaker
- `README.md`: this file

## Credits

Built with the [ESP8266Audio](https://github.com/earlephilhower/ESP8266Audio) library for MP3 decoding and [TJpg_Decoder](https://github.com/Bodmer/TJpg_Decoder) for album covers. Display driver from Adafruit.
