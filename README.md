# Levitate

[![Build](https://github.com/LiteLDev/Levitate/actions/workflows/build.yml/badge.svg)](https://github.com/LiteLDev/Levitate/actions/workflows/build.yml)
[![License](https://img.shields.io/github/license/LiteLDev/Levitate)](COPYING.LESSER)

Rotational power, gears, machines and moving contraptions. Levitate brings simibubi and the Creators of Create's Java mod [Create](https://modrinth.com/mod/create) to Bedrock, for dedicated servers and single-player worlds.

> [!NOTE]
> Levitate is in early development. There is no release yet.

## Install

You need Minecraft Bedrock Edition **1.26.51** for Windows (x64) with LeviLamina **26.51.x** (on a client, installed through [LeviLauncher](https://github.com/LiteLDev/LeviLauncher)).

With [lip](https://github.com/LiteLDev/lip):

```sh
lip install github.com/LiteLDev/Levitate           # dedicated server
lip install github.com/LiteLDev/Levitate#client    # client
```

Or download the zip from the [latest release](https://github.com/LiteLDev/Levitate/releases/latest) and extract it next to `LeviLamina/`:

| Where | Zip | Folder |
| --- | --- | --- |
| Dedicated server | `levitate-server-windows-x64.zip` | `plugins/Levitate/` |
| Client (LeviLauncher) | `levitate-client-windows-x64.zip` | `mods/Levitate/` |

## Commands

| Command | Who | What it does |
| --- | --- | --- |
| `/create reload` | Operators, console | Reloads the configuration and translations. |
| `/create selftest ["glob"]` | Operators, console | Runs the in-game selftests. |

`/levitate` is an alias of `/create`.

## Configuration

Settings live in `config/*.json` inside the mod folder (`plugins/Levitate/` or `mods/Levitate/`). Each file is created with defaults on first start.

| File | Setting | Default | Meaning |
| --- | --- | --- | --- |
| `mod.json` | `language` | `en_US` | Language of server messages when a player's own language has no translation. |
| | `usePlayerLocale` | `true` | Use each player's game language when a translation exists. |
| | `requireResourcePack` | `true` | Dedicated server: players must accept the resource pack to join. |

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). [AGENTS.md](AGENTS.md) is the developer guide.

## Credits

Levitate reproduces the behaviour of [Create](https://github.com/Creators-of-Create/Create) by simibubi and the Creators of Create. It copies no code or assets from it.

## License

[LGPL-3.0-or-later](COPYING.LESSER)
