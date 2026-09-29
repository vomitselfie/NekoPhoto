# Bundled ICC profiles

## ISOcoated_v2_300_bas.ICC

**ISO Coated v2 300% (basICColor)**, a FOGRA39 characterisation (ISO 12647-2 offset on coated paper, total area
coverage 300%) by basICColor GmbH. NekoPhoto's default Working CMYK (docs/color-management.md), compiled into the core
(`compositor::defaultCmykProfile()`).

- Licence: zlib/libpng, copyright (c) 2007-2010 basICColor GmbH: [LICENSES/basICColor-zlib.txt](../../../LICENSES/basICColor-zlib.txt).
- Source: the `icc-profiles-basiccolor-printing2009` folder of Debian's `icc-profiles-free` 2.4 source package
  (`default_profiles/printing/ISOcoated_v2_300_bas.ICC`, with its `LICENSE-ZLIB-bICC`), which packages the
  basICColor printing set from colormanagement.org (Kai-Uwe Behrmann's OpenICC distribution). Debian's
  `debian/copyright` lists the folder under `License: Zlib`.
- SHA-256: `b424c77f40c3423c925536f8ae08634985ccd0fe80eb253d5d229197ded7e886` (1 052 612 bytes), unmodified.
