# Verified AppImage packaging tools

The packaging script obtains binaries only from their official upstream release URLs and verifies
SHA-256 before running them. A moving `continuous` asset may change; an expected checksum failure
must be reviewed against upstream before updating the checked-in default, rather than bypassed.

Validated on 2026-10-01:

| Tool | Upstream build | SHA-256 |
| --- | --- | --- |
| appimagetool | AppImage/AppImageKit, commit 5735cc5 | b90f4a8b18967545fda78a445b27680a1642f1ef9488ced28b65398f2be7add2 |
| linuxdeploy | linuxdeploy/linuxdeploy, commit 07333c6, build 369 | 8aea8da0f7f7039d2a2cecb14657d752a222a5e1d3825caeef186c82f751cdd1 |
| linuxdeploy-plugin-qt | linuxdeploy/linuxdeploy-plugin-qt, commit 9b9fca1, build 256 | cfc1055b2b9dbc08412b579f20990b7b41a17b61beaa5847dc9477c96c9e9617 |

To use a different reviewed build, override its URL and matching SHA-256 together. No release is
published by the local packaging scripts; they write only under `dist/`.
