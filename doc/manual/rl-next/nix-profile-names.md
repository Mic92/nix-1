---
synopsis: "`nix profile` now allows referring to elements by human-readable name, and no longer accepts indices"
prs: 8678
cls: [978, 980]
category: Breaking Changes
credits: [iFreilicht, Qyriad, edolstra]
---

[`nix profile`](@docroot@/command-ref/new-cli/nix3-profile.md) now uses names to refer to installed packages when running [`list`](@docroot@/command-ref/new-cli/nix3-profile-list.md), [`remove`](@docroot@/command-ref/new-cli/nix3-profile-remove.md) or [`upgrade`](@docroot@/command-ref/new-cli/nix3-profile-upgrade.md) as opposed to indices. Indices have been removed. Profile element names are generated when a package is installed and remain the same until the package is removed.

**Warning**: The `manifest.nix` file used to record the contents of profiles has changed. Lix will automatically upgrade profiles to the new version when you modify the profile. After that, the profile can no longer be used by older versions of Lix.
