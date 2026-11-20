#!/usr/bin/env bash
# Full source-build dependency set for the Debian 13 media SDK producer.
set -euo pipefail
sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/debian.sources
apt-get update
apt-get build-dep -y --no-install-recommends ffmpeg
apt-get install -y --no-install-recommends \
  ca-certificates git build-essential pkgconf python3 curl patch xz-utils unzip \
  ffmpeg glslc libmpv-dev python3-glad python3-jinja2 libfast-float-dev \
  libvulkan-dev libshaderc-dev liblcms2-dev autoconf automake libtool nasm \
  libfreetype-dev libfontconfig1-dev libfribidi-dev libharfbuzz-dev libunibreak-dev
