# logos-package-downloader

C++ library and CLI (`lgpd`) for fetching **Logos module catalogs** and
downloading `.lgx` packages — across **multiple repositories**.

This repo handles **network / download** operations only. It does not
install packages — that is the responsibility of
[logos-package-manager](https://github.com/logos-co/logos-package-manager).

## Model

A *repository* is identified by the URL of its `logos-repo.json` (its
identity card: name, `indexUrl`, trusted signers). The downloader keeps a
registry of repositories — a built-in default plus any the user adds —
fetches each one's `index.json`, and **merges them into a single
catalog**. Packages are downloaded by their per-version `url` and
**verified against the catalog entry** before they're handed off for
install (content `rootHash` + manifest fields + signer DID).

> 📖 The `logos-repo.json` and `index.json` formats this library consumes
> are fully specified in
> [logos-modules-release-tool/docs/catalog-format.md](https://github.com/logos-co/logos-modules-release-tool/blob/main/docs/catalog-format.md).
> To build/host your own catalog, see
> [logos-modules-release-base](https://github.com/logos-co/logos-modules-release-base).

## Library API

The public API speaks JSON strings so it's trivially callable from a C
wrapper, the package-downloader Logos module, and tests.

```cpp
#include <package_downloader_lib.h>

// In-memory only, or backed by a config file that persists user repos.
PackageDownloaderLib dl;                          // built-in default repo only
PackageDownloaderLib dl{"/path/repositories.json"}; // + persisted user repos

// ── Repositories ────────────────────────────────────────────────
// The default repo is always present; user repos are added by the URL
// of their logos-repo.json and persisted (when constructed with a path).
dl.registry().addRepository("https://example.com/my/logos-repo.json");
dl.registry().setEnabled("https://example.com/my/logos-repo.json", false);
dl.registry().removeRepository("https://example.com/my/logos-repo.json");
std::string repos = dl.listRepositoriesJson();    // [{url,enabled,isDefault,name,…,resolveError}]

// ── Catalog ─────────────────────────────────────────────────────
// Each row: {repositoryUrl,name,displayName,type,category,…,variants,
//            reportUrl,universalLink,versions}.
// `variants` is the newest version's platform-variant list (sorted, always
// present) — desktop, mobile and web names from logos-package's vocabulary —
// so a caller can answer "will this run on my device" before downloading.
// `reportUrl` and `universalLink` are the per-module links a store shell needs:
// the package's own values when it declares them, else the repository's
// `reportUrlTemplate` / `universalLinkTemplate` with `{name}` and `{version}`
// expanded. Always present, empty when nothing declared them.
std::string all   = dl.getCatalogJson();          // merged across enabled repos
std::string oneEl = dl.getCatalogForRepoJson("my-catalog"); // one repo (url or name)
dl.refreshCatalogs();                              // force re-fetch of repo metadata + indexes

// ── Dependency resolution (no download) ─────────────────────────
// Input: a manifest-style dependency list. Output: resolved versions in
// install order, each tagged `topLevel`. The optional installed-packages
// snapshot lets the resolver omit transitive deps already satisfied
// on-disk.
std::string plan = dl.resolveDependenciesJson(R"([{"name":"chat_module"}])");
std::string plan2 = dl.resolveDependenciesJson(depsJson, installedJson);

// ── Download (pinned + verified) ────────────────────────────────
// repoUrlOrName empty → any enabled repo (registry order); version empty
// → newest matching; rootHash disambiguates two builds sharing a version.
// Returns the local .lgx path, or empty on error.
std::string path = dl.downloadPackage(/*repo*/"", "wallet_module",
                                      /*version*/"1.0.0", /*rootHash*/"",
                                      /*outputDir*/"/tmp/pkgs");
```

### C API

A C-compatible API is available via `lgpd.h` for non-C++ consumers. All
returned `char*` are owned by the caller — free with `lgpd_free_string`;
errors via `lgpd_get_last_error()`.

```c
#include <lgpd.h>

lgpd_context_t ctx = lgpd_create();                    // or lgpd_create_with_config(path)
lgpd_repo_add(ctx, "https://example.com/my/logos-repo.json");

char* catalog = lgpd_get_catalog(ctx);                 // merged catalog JSON
lgpd_free_string(catalog);

char* plan = lgpd_resolve_dependencies(ctx, "[{\"name\":\"chat_module\"}]");
lgpd_free_string(plan);

char* path = lgpd_download_package(ctx, /*repo*/"", "wallet_module",
                                   /*version*/"1.0.0", /*root_hash*/"",
                                   /*output_dir*/"");
lgpd_free_string(path);
lgpd_free(ctx);
```

## CLI (`lgpd`)

```
lgpd [global options] <command> [arguments]

Catalog commands:
  list                          List all available packages
  search <query>                Search packages by name or description
  info <package>                Show package details (all versions, by date)
  download <package>            Download a .lgx package

Repository management (require --config <path>):
  repo list                     Show configured repositories
  repo add <url>                Add a user repository (url -> logos-repo.json)
  repo remove <url>             Remove a user repository
  repo enable <url>             Enable a repository
  repo disable <url>            Disable a repository
  repo refresh                  Re-fetch every repo's logos-repo.json + index.json

Config:
  config init <path>            Create an empty repository config file
  config show                   Print the resolved config JSON
  config path                   Print the current --config path

Global options:
  --config <path>               Path to repositories.json (required for repo mutations)
  --repo <url-or-name>          Restrict a catalog/download command to one repo
  --version <ver>               Pin a specific package version (download/info)
  --root-hash <hex>             Disambiguate two releases sharing a version
  --category <cat>              Filter by category (list)
  -o, --output <dir>            Output directory (download)
  --json                        Emit structured JSON
  -h, --help                    Show help
  -V, --version                 Print version
```

### Examples

```bash
# Browse the merged catalog
lgpd list
lgpd search waku
lgpd info wallet_module --json

# Add and manage your own repository (persisted to the config file)
lgpd --config ~/.config/logos/repositories.json repo add https://example.com/my/logos-repo.json
lgpd --config ~/.config/logos/repositories.json repo list
lgpd --config ~/.config/logos/repositories.json repo refresh

# Download — pin a version, scope to one repo, choose an output dir
lgpd download wallet_module --version 1.0.0 --repo my-catalog -o ./packages/
```

## How to Build

### Using Nix (Recommended)

The downloader ships as a C++ library plus the `lgpd` CLI. `nix build` (no target) produces the combined library + CLI.

#### Local Build

A standard Nix derivation whose dependencies live in `/nix/store`. It is the fastest way to iterate during development but is **not portable** — it only runs on the machine that built it.

```bash
nix build                        # library + CLI (combined)
nix build '.#lib'                # library only
nix build '.#cli'                # CLI only
./result/bin/lgpd --help
```

#### Portable Builds

Portable builds are **fully self-contained** — no `/nix/store` references at runtime — for distribution. Unlike [logos-package-manager](https://github.com/logos-co/logos-package-manager), `lgpd` only fetches files over the network and has no dev variant/portable *variant* distinction; downloaded packages are always portable.

| Output | Platform | Format |
|---|---|---|
| `cli-bundle-dir` | Linux, macOS | Self-contained flat directory with `bin/` and `lib/` |
| `cli-appimage` | Linux | Single-file `.AppImage` executable |

##### Self-contained directory bundle (all platforms)
```bash
nix build '.#cli-bundle-dir'
./result/bin/lgpd --help
```

##### Linux AppImage (Linux only)
```bash
nix build '.#cli-appimage'
./result/lgpd.AppImage --help
```

#### Development Shell

```bash
nix develop
```

**Note:** In zsh, quote targets containing `#` to prevent glob expansion (e.g., `'.#cli'`).

If you don't have flakes enabled globally:

```bash
nix build --extra-experimental-features 'nix-command flakes'
```

## Testing

```bash
nix flake check                  # run all tests
nix build .#tests                # build and run tests
```

## Dependencies

- `nlohmann_json` — JSON parsing
- `libcurl` — HTTP operations
