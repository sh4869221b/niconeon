# Verified AppImage packaging tools

The packaging script downloads specific official GitHub release **asset IDs**, using
`Accept: application/octet-stream` and following redirects, and verifies the checked-in
SHA-256 before running them. It never discovers a new digest or falls back to `continuous`
at packaging time. A cached tool is also verified before reuse; a mismatched download stops
packaging before any tool executes.

Validated on 2026-11-06 (downloaded bytes match these SHA-256 values):

| Tool | Official asset metadata | Upstream build | SHA-256 |
| --- | --- | --- | --- |
| appimagetool | [98605504](https://api.github.com/repos/AppImage/AppImageKit/releases/assets/98605504) | commit 5735cc5; unchanged binary | b90f4a8b18967545fda78a445b27680a1642f1ef9488ced28b65398f2be7add2 |
| linuxdeploy | [699514588](https://api.github.com/repos/linuxdeploy/linuxdeploy/releases/assets/699514588) | commit 07333c6, build 373 | df908b19f1ed1cdafa79508d415aa12d6c56ce4d56d4149aa8c09286718706e5 |
| linuxdeploy-plugin-qt | [525032210](https://api.github.com/repos/linuxdeploy/linuxdeploy-plugin-qt/releases/assets/525032210) | commit 9b9fca1, build 256; unchanged binary | cfc1055b2b9dbc08412b579f20990b7b41a17b61beaa5847dc9477c96c9e9617 |

The linuxdeploy and Qt plugin hashes also match the GitHub API `digest` fields. The older
appimagetool asset has no API digest; its downloaded bytes still match the independently
checked-in 2026-10-01 hash. Asset IDs avoid following replacement uploads, but upstream can
delete an asset: a 404 or changed bytes must fail, not silently select another build.

## 2026-11-06 checksum failure investigation

The old linuxdeploy pin was build 369, SHA-256
`8aea8da0f7f7039d2a2cecb14657d752a222a5e1d3825caeef186c82f751cdd1` (19,806,712 bytes).
It was verified against the AppImage inside official [build 369](https://github.com/linuxdeploy/linuxdeploy/actions/runs/36815113236),
artifact `11141506107`. The moving `continuous` URL now serves build 373, uploaded
2026-11-01 (19,855,864 bytes), whose actual hash matches the digest of asset `699514588`
in [release 430855031](https://api.github.com/repos/linuxdeploy/linuxdeploy/releases/430855031).
The release links to successful [build 373](https://github.com/linuxdeploy/linuxdeploy/actions/runs/41349296615).
Both runs use source commit `07333c6e942c7d71782b66be924c5d867f9dfdfc`.

The [upstream workflow](https://github.com/linuxdeploy/linuxdeploy/blob/07333c6e942c7d71782b66be924c5d867f9dfdfc/.github/workflows/main.yml)
rebuilds monthly, and its [build script](https://github.com/linuxdeploy/linuxdeploy/blob/07333c6e942c7d71782b66be924c5d867f9dfdfc/ci/build.sh)
fetches build inputs that are not all commit-pinned. Equal source commits therefore do not
imply equal binary bytes. This update checks official release/run/asset identity and hashes;
it does not claim signed build provenance or a reproducible rebuild. The checksum gate that
caught the replacement remains enabled. The previous source pin is retained instead of
downgrading to the older source in the latest named linuxdeploy release.

The [GitHub release asset API](https://docs.github.com/en/rest/releases/assets#get-a-release-asset)
supports these public downloads without credentials. Tests cover verified cache reuse,
replacement of corrupt caches, the binary request header, and rejection of corrupt downloads.

To use a different reviewed build, override its URL and matching SHA-256 together. No release is
published by the local packaging scripts; they write only under `dist/`.
