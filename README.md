# Flexi-Counter – ESP32 Factory Production Counter

ESP32-based production counting and reporting experiments for factory equipment. The repository includes an earlier Arduino sketch and an ESP32-S3 firmware variant with Firebase synchronization, local storage/retry features, and industrial-network configuration.

## Source files

- `counterok.ino` — earlier Arduino counter sketch
- `counterveriso2` — ESP32-S3 firmware variant; review target board and rename/move into an Arduino sketch folder as needed before building
- `secrets.example.h` — placeholder local configuration; copy into `secrets.h`
- `.gitignore` — excludes local `secrets.h` and generated builds

## Setup

1. Select the **correct board and sketch version** before editing or flashing any device.
2. Create `secrets.h` from `secrets.example.h` in the sketch's include path.
3. Fill in the new Wi-Fi credentials and any device/Firebase configuration required by the selected firmware. Never commit this file.
4. Install the Arduino/ESP32 board core and dependencies matching the source includes.
5. Compile locally and test reset/count behavior, Firebase connectivity, time sync and recovery from Wi-Fi outages before deployment to a production machine.

Both `counterok.ino` and `counterveriso2` now refer to the ignored local `secrets.h` for credentials. Supply the definitions from `secrets.example.h` and verify the Arduino build before flashing; merging the source does not update deployed devices.

## Security notice

**A Wi-Fi password and other device configuration were committed in earlier revisions.** Removing strings from the current branch does not remove Git history. Rotate any real Wi-Fi passwords and device authentication secrets, review Firebase security rules, and regenerate deployed firmware credentials. A Firebase client API key is normally a public project identifier, not a replacement for database access controls.

## Operational safety

Avoid flashing untested firmware to running production equipment. Preserve a known-good binary, verify sensor wiring and pin assignments, and test count persistence and network failure recovery in a controlled environment.

## Status

Documentation only; firmware has not been compiled, flashed or validated through this GitHub change.
