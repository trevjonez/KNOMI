# Reproducible build environment for the KNOMI firmware and its generated art.
#
# Two toolchains live here so the whole pipeline runs the same way everywhere:
#   * PlatformIO + the ESP32 toolchain, to build the firmware
#   * pillow / numpy / scipy, to regenerate the StealthBurner GIF assets
#
# The firmware's own reproducibility comes from platformio.ini, which pins
# platform = espressif32@6.4.0 and every lib_deps entry to a tag or zip.
FROM python:3.11-slim-bookworm

# git: PlatformIO resolves several lib_deps straight from GitHub, and
# tools/stealthburner/build_gifs.py reads the source art out of the repo.
RUN apt-get update \
 && apt-get install -y --no-install-recommends git ca-certificates \
 && rm -rf /var/lib/apt/lists/*

RUN pip install --no-cache-dir \
      "platformio>=6.1,<7" \
      pillow numpy scipy

# Bake the ESP32 toolchain into the image rather than downloading it on first
# use. World-writable so the container can run as the invoking UID (see run.sh)
# and still write PlatformIO's caches.
ENV PLATFORMIO_CORE_DIR=/pio
RUN pio pkg install --global --platform espressif32@6.4.0 \
 && chmod -R a+rwX /pio

# The repo is bind-mounted, so its owner won't match the container user.
RUN git config --system --add safe.directory '*'

WORKDIR /repo
