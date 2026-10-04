# Provenance

| Field | Value |
| --- | --- |
| Canonical upstream | `https://gitlab.freedesktop.org/wlroots/wlroots.git` |
| Upstream base | `88a869855742281c98c22cab9641b317b8d065ef` |
| Licence | `LICENSE` |
| PocketForge patch | Pin Meson fallback projects to immutable PocketForge fork commits. |

The PocketForge patch series is based directly on the upstream commit above. It
does not modify wlroots runtime source or change which fallback projects are
available.

The `libdisplay-info` fallback is pinned to PocketForge commit
`0791e6f6cfa928f85b952806811f3df998d2e898`, the reviewed locator patch based on
upstream `47a5590e9c4eb35d67651b8c05a55f1a48259329` (version 0.3.0). A forced
fallback build of wlroots' DRM backend verifies that this shared patch line is
compatible, avoiding a second patch branch at the newer 0.5.0 development pin.
