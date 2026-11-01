# WotLK localization

The expansion profile's `locale` selects both the authentication locale and
`GetLocale()`. For build 12340, DBC readers select explicitly described locstring
columns in the requested language, falling back to primary text with a warning.
Asset paths and numeric fields retain their original meaning.

An optional language pack lives in `Data/expansions/wotlk/locales/ruRU` with its
own `manifest.json`. Extract it from an installed client on Windows:

```powershell
$env:TMP = $env:TEMP = $env:TMPDIR = 'D:\games\WoWee\build-dev\tmp'
.\build-dev\bin\asset_extract.exe --mpq-dir D:\games\Isengard_WotLK_335a\Data `
  --output D:\games\WoWee\Data\expansions\wotlk\locales\ruRU `
  --expansion wotlk --locale ruRU --locale-only
```

`--locale-only` reads only archives inside the selected locale directory, in
normal patch precedence. It requires an explicit locale. Do not extract this
subset over the base game's manifest: it does not contain the world.

The runtime uses the pack's interface, fonts and audio. FrameXML and its Blizzard
addons use the locale interface directory. Language archives may also contain
executables and world models; those are never activated by the locale layer.
User overrides and mounted realm archives keep their existing resource priority.

DBC tables merge only the requested language's strings, by existing record ID.
They do not add rows or replace gameplay fields. Existing localized text wins;
realm diff tables are applied afterwards. This supports an English base client
without replacing its spell rules with those from the translation's source.
The pack must match the base build and table column counts.

The base extraction remains required. The locale pack was extracted from the
installed Isengard ruRU client and its manifest CRCs checked. After restoration
of the base dataset on 2026-10-22, it contained 230,177 indexed files. Its German
FrameXML is superseded by the ruRU pack; `framexml_run` selects the same localized
interface and fonts as the client.

## Schema provenance

`src/pipeline/dbc_locale_fields.inc` is generated from the locstring definitions
in [WoWDBDefs](https://github.com/wowdev/WoWDBDefs/tree/7eaa562b62037d12327543fe22719a59870bb905),
commit `7eaa562b62037d12327543fe22719a59870bb905`, build `3.3.5.12340`.
The generated schema data is adapted from that project's definitions and remains
under [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/).
Changes: retain only localized column offsets and total column counts; emit C++.
The generator is `tools/generate_dbc_locales.py`:

```sh
python3 tools/generate_dbc_locales.py /path/to/WoWDBDefs/definitions src/pipeline/dbc_locale_fields.inc
```

The generated metadata covers 68 tables; their shapes were checked against the
original ruRU DBC files. Adding a build requires verified definitions for that
build. The tool rejects unsupported field shapes rather than guessing offsets.

## Verification

`test_dbc_loader` checks localized reads, fallback, realm string-block rebasing
and merging translations without importing changed gameplay values or new IDs.
`tests/framexml_macro_icons.lua` runs through `framexml_run` and original FrameXML
with learned spells and actual icon tables; it checks macro and action-button
icons and editing, including localized spell names. UTF-8 tests are supplied with
`--lua-file:path` to avoid Windows command-line codepage conversion. The same
checks were also run with English base spell names and a separate ruRU pack. The macro regression failed
before the resolver was implemented; the locale merge test failed before merging
was implemented.
