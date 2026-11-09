# Quirk TODO

Bigger pieces of work, roughly in priority order. Small fixes go straight
into a release; this list is for things that need a plan.

## 1. Package discovery

The registry (`quirk-pkg/registry`) exists, but there's no way to browse it.

- `quirk search <term>`: match names and descriptions in the registry.
- `quirk show <pkg>`: also print the package's README and latest version.
