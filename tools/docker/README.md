# Reproducible build environment

Builds the KNOMI firmware, and regenerates the StealthBurner art, inside a
container — so nothing (PlatformIO, the ESP32 toolchain, pillow/numpy/scipy)
has to be installed on the host.

```sh
./tools/docker/run.sh            # build the KNOMI2 firmware
./tools/docker/run.sh assets     # regenerate the GIFs and their .c files
./tools/docker/run.sh shell      # poke around
./tools/docker/run.sh pio run -e knomiv1
```

The image is built automatically on first use; that step downloads the ESP32
toolchain (~1GB) and bakes it into the image, so later runs are offline apart
from the project's own `lib_deps`.

Firmware output lands in `.pio/build/knomiv2/firmware.bin` in the working
tree. The container runs as the invoking user, so generated files are owned
by you rather than root.

## What's pinned where

The image pins the *tooling*; the firmware's reproducibility comes from
`platformio.ini`, which already pins `platform = espressif32@6.4.0` and every
`lib_deps` entry to a tag or a zip. That split is deliberate — bumping the
image shouldn't change the firmware, and vice versa.

`pio pkg install --global --platform espressif32@6.4.0` pre-installs the
toolchain into `/pio` (`PLATFORMIO_CORE_DIR`), made world-writable so the
container still works when run as an arbitrary UID.

## Flashing

The container builds; it doesn't flash. The KNOMI2 takes firmware over OTA:

    http://knomi.local/update

Back up the running firmware first — there's no downgrade path except
reflashing over USB with Espressif's Flash Download Tool.
