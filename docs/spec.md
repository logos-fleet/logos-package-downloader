# Logos Package Downloader Specification

## Overview

The Logos Package Downloader is the **network / download tier** of the Logos
package-management stack. It answers two questions for anything that wants a
Logos module: *what packages exist?* and *give me this one, verified.* It
browses online catalogs of Logos modules spread across one or more
repositories, merges them into a single view, resolves cross-repository
dependency graphs, and downloads `.lgx` package artifacts — checking each one
against the catalog that advertised it before handing it off.

It deliberately does **one half** of the job. It fetches and verifies; it does
**not** install. Installation — unpacking a `.lgx`, placing plugins, checking
the signer against a trust keyring — is the responsibility of its sibling, the
Logos Package Manager (`lgpm`). The two form a pipeline:

```
   ┌─────────────────────┐      .lgx file       ┌─────────────────────┐
   │  package downloader │ ───────────────────► │   package manager   │
   │      (lgpd)         │   (verified vs       │      (lgpm)         │
   │                     │    catalog)          │                     │
   │  fetch · merge ·    │                      │  install · trust    │
   │  resolve · verify   │                      │  check · place      │
   └─────────────────────┘                      └─────────────────────┘
        network tier                                 install tier
```

The same capabilities are offered three ways — as a command-line tool
(`lgpd`), as a programmatic library, and as a C-callable boundary — so the
desktop app, a headless script, an AI agent, and the in-platform
package-downloader module can all drive the identical logic. The public surface
speaks **JSON strings** rather than typed records, precisely so every one of
those callers consumes the same shapes without an adapter layer.

### Where it sits in the platform

```
   logos-modules-release-*  ──►  publishes  logos-repo.json + index.json
                                                  │  (HTTPS catalog)
                                                  ▼
   logos-package-downloader  ──►  browse / resolve / download  ──►  .lgx
                                                  │
                                                  ▼
   logos-package-manager     ──►  install (trust-checked) into a runtime
                                                  │
                                                  ▼
   logoscore / logos-basecamp  ──►  load the installed modules
```

A *repository* is produced by the release tooling
(`logos-modules-release-tool` / `logos-modules-release-base`). The downloader
is purely a **consumer** of those catalog formats; it never writes them.

---

## Domain Model

The model is **multi-repository**. The downloader does not assume a single
source of truth — it federates several and presents them as one.

| Concept | Meaning |
|---------|---------|
| **Repository** | A package source, identified by the URL of its `logos-repo.json`. That document is the repository's identity card: canonical `name`, human `displayName`, `description`, `homepage`, the `indexUrl` where its package index lives, a list of `trustedSigners` (each with a `did`), and optionally the two per-module **link templates** below. |
| **Link templates** | `reportUrlTemplate` and `universalLinkTemplate` in `logos-repo.json`, with `{name}` and `{version}` expanded per catalog row into the row's `reportUrl` and `universalLink`. A store shell needs both per module — where to report one that misbehaves, and a link that addresses one from outside the app — and a catalog with three hundred modules should not repeat one URL three hundred times. A package may override either with its own explicit `reportUrl` / `universalLink` in the index; a non-string value there falls through to the template rather than reaching a consumer as a link it would open. |
| **Registry** | The set of repositories the client knows about: one hardcoded **default repository** plus any number of **user repositories**. The registry is what `add`/`remove`/`enable`/`disable` operate on. |
| **Index** | A repository's package index (`index.json`): a list of packages, each with one or more **versions**. Each version carries a `releasedAt` date, the download `url` of its `.lgx`, size/checksum fields, a `rootHash`, an embedded `manifest`, and an optional `signature`. |
| **Catalog** | The merged, synthesised view across all *enabled* repositories. This is the unit the browse/search/info/resolve operations work against. |
| **Package** | A named module available in one or more repositories, with a version history. |
| **Version** | A concrete release of a package, pinned by its `version` string and its `rootHash`. |
| **`rootHash`** | The content Merkle root of a package version. Pins exactly which bytes a version refers to, and disambiguates two builds that happen to share a `version` string. |
| **Manifest** | The package's own declaration of identity (`name`, `version`), entry points (`main`), `dependencies`, `type`, and descriptive fields. Embedded in the index, and also carried inside the `.lgx` itself. |
| **Signer DID** | A decentralised identifier naming who signed a version. The downloader binds the advertised signer to the downloaded file; it does **not** evaluate whether that signer is *trusted* (that is the package manager's job). |
| **`trustedSigners`** | What a repository advertises about its own signers in `logos-repo.json`. **Advisory display data** (logos-workspace ADR 0008): parsed into `Repository::trustedSignerDids`, echoed by `listRepositoriesJson`, and consulted by nothing. It is a repository's self-assertion about itself, fetched from the same server as everything else it says, so it authorises nothing — the only trust-anchor set is the *local keyring*, which nothing enters except by an explicit user act. Its one honest job is to supply the candidate for a "this repository publishes as X — add X to your keyring?" affordance: a candidate, shown to a person, who acts. Never a decision, and never an input to one. |

### The default repository

The registry always contains a single built-in repository — the official
logos-co modules release — even with no configuration at all. This is what
makes `lgpd list` work out of the box. The default repository has special
rules:

- It is present in the merged view by default.
- It can be **disabled** (`defaultDisabled` — row stays with `enabled=false`)
  or **removed** (`defaultRemoved` — row omitted). Re-adding
  `kDefaultRepositoryUrl` restores it. It is never stored as a user repo.
- It is **never written** into the user config's repository list; only user
  repositories are persisted. Its identity is compiled in.

### Configuration

User repositories are persisted to a JSON config file with a stable schema:

```
{ "schemaVersion": 1,
  "defaultDisabled": false,
  "defaultRemoved":  false,
  "repositories": [ { "url": "...", "enabled": true }, ... ] }
```

Only the **bare facts** — each repository's URL and whether it is enabled, plus
the default-disabled flag — are stored. All resolved metadata (name,
displayName, indexUrl, signers) is **fetched at runtime** from each
`logos-repo.json`, never cached to disk. Without a config file the client runs
**in-memory**: the default repository works, but there is nowhere to persist
user repositories, so the mutating repository operations are refused.

---

## Catalog Resolution

When the client needs the catalog, it performs **metadata resolution** followed
by an **index merge**:

```
   for each repository in the registry:
       fetch logos-repo.json       ──► resolve name, indexUrl, signers
                                        (failure recorded per-repo, not fatal)

   for each ENABLED repository that resolved cleanly:
       fetch index.json
       synthesise one catalog entry per package:
           + repositoryUrl / repositoryName / repositoryDisplayName
           + header fields (description, type, category, author, icon)
             lifted from the first version's manifest
           + versions[]  sorted newest-first by releasedAt

   concatenate all entries  ──► the merged catalog
```

Two properties matter:

1. **Best-effort federation.** A repository that fails to fetch or parse is
   recorded with a per-repository `resolveError` and **skipped** — it does not
   abort the whole catalog. One unreachable repository never blackouts the
   others.
2. **Lazy, cached metadata.** Metadata resolution happens **once per process**
   on first use. A long-lived caller sees a stable view until it explicitly
   asks for a refresh; a stale view is corrected by `repo refresh` / the
   refresh operation, which clears caches and re-fetches everything.

A single-repository view (the same synthesised shape, scoped to one repository
by URL or canonical name) is available for callers that want to browse one
source in isolation.

---

## Verified Download

A download is **pinned and verified** — never a blind fetch. The flow:

```
   1. SELECT  the version:
        repo empty      → any enabled repository, in registry order
        version empty   → newest version of the package
        rootHash given  → must match exactly (disambiguates same-version builds)

   2. FETCH   the .lgx by the version's own url
        output dir empty → system temp (TMPDIR, else /tmp)
        a failed transfer leaves no half-written file behind

   3. VERIFY  the downloaded file against the catalog entry that named it
        any mismatch  → delete the artifact, fail the download

   4. RETURN  the local path to the verified .lgx
```

### Index → file binding

The index comes from one host; the `.lgx` blob comes from wherever its `url`
points — often a different host. Nothing in transit stops a **swapped file** (a
downgrade attack, or a different-but-also-signed sibling package). Verification
binds the two together along four facets:

| Facet | Check |
|-------|-------|
| **Structure** | The file is a well-formed `.lgx` whose internal content hashes are self-consistent. Catches a truncated/corrupt download, or a non-`.lgx` served at the URL. Runs unconditionally. |
| **Content (`rootHash`)** | The `rootHash` the catalog pinned must equal the `.lgx` manifest's recorded content root. A plain identity compare binds the download to the advertised content. |
| **Manifest fields** | The advertised manifest's `name`, `version`, `main`, `dependencies`, and `type` must match the file's manifest. These are the fields a malicious swap would have to alter to matter. |
| **Signer** | If the catalog advertised a signer DID, the file must be signed by the **same** DID. |

A facet is **skipped when the catalog advertised nothing to compare against** —
e.g. legacy index rows with a null manifest or a missing `rootHash`. The
principle is *you cannot verify what was never advertised*; such rows pass that
facet rather than being rejected.

What this is **not**: deep cryptographic trust. The downloader confirms *the
file is what the index said it would be* (same DID, same content, same
manifest). Whether that signer is in your keyring — the Ed25519 trust decision
— is intentionally **out of scope** and stays the package manager's job at
install time.

---

## Dependency Resolution

The downloader can plan an install order across repositories **without
downloading anything**. Given a starting list of dependencies (in the manifest
dependency form — a bare name, or an object with `name` plus optional `version`
range, `signer`, and `repositoryUrl` scope), it returns the resolved versions
in **install order** (dependencies before the packages that need them, no
duplicates).

```
   input: [{ "name": "chat_module" }]      (the packages you explicitly want)
              │
              ▼  breadth-first across the merged catalog
   for each dependency:
       pick the newest catalog version whose
         version meets the range, signer matches (if pinned),
         and repository matches (if pinned)
       enqueue that version's own manifest dependencies (transitive)
              │
              ▼
   output (install order):
   [ { repositoryUrl, name, version, rootHash, url, topLevel }, ... ]
```

Key behaviors:

- **`topLevel` marking.** Entries that came from the caller's input array are
  tagged `topLevel: true` — the packages explicitly requested. Transitive
  dependencies the resolver pulled in are `topLevel: false`. Consumers use this
  to separate "the thing I asked for" from "the things that come along."
- **Installed-state short-circuit.** The caller may pass a snapshot of what is
  already installed (`[{ name, version, rootHash }]`). A **transitive**
  dependency already satisfied on disk (its installed version meets the range)
  is **omitted entirely** from the output — no install, no change. Top-level
  entries are never short-circuited: the caller picked them explicitly, so they
  always resolve to a fresh catalog pick.
- **Cross-repository tie-breaking.** When the same package name is published by
  more than one repository, the newest version (by `releasedAt`) across all of
  them wins — unless a dependency pins a `repositoryUrl`, which scopes the
  search to that one source.
- **Unsatisfiable constraints.** If a dependency cannot be satisfied, the output
  contains an `{ error, name }` entry at the unsatisfied position and
  resolution stops. Callers must check for `error`.

### Version range matching

Ranges are matched with an **npm/Cargo-flavoured semver subset**, covering the
forms the platform's catalogs use:

| Form | Example | Meaning |
|------|---------|---------|
| Exact | `1.2.3` | Exactly that version |
| Comparators | `>=1.0.0`, `<2.0.0`, `>1.2`, `<=3.0` | Ordered comparison |
| Caret | `^1.2.3` | Compatible within the leftmost non-zero element |
| Tilde | `~1.2.3` | Patch-level (or minor-level) updates only |
| Wildcards | `*`, `1.x`, `1.2.x` | Any segment |
| Conjunction | `>=1.0 <2.0` | Whitespace-separated comparators all match |
| Alternation | `1.x \|\| 2.x` | Any alternative matches |
| Empty | `` | Matches anything (no constraint) |

This is intentionally **not** full semver 2.0: build metadata is ignored for
comparison, and pre-release ordering only honours the basic rule that a
pre-release sorts before its corresponding release.

---

## Capabilities & Functional Requirements

The system exposes the same conceptual operations across the CLI, the library,
and the C boundary.

### Catalog operations (no configuration required)

| Operation | Behavior |
|-----------|----------|
| **List** | Return all packages in the merged catalog (or, scoped to one repository, just that source). May be filtered by category (case-insensitive). |
| **Search** | Case-insensitive substring match over each package's name and description, across the merged catalog (or one repository). |
| **Info** | Return a single package's details and its full version history, sorted newest-first — each version shown with its version string, release date, `rootHash`, and whether it is signed. |
| **Download** | Fetch and verify a single package version, returning the local `.lgx` path. Selection rules: repository optional (any enabled), version optional (newest), `rootHash` optional (disambiguates), output directory optional (system temp). |

### Repository management (mutations require a config file)

The mutating operations (**add / remove / enable / disable**) require a config
file and are refused by an in-memory client. **Repo list** and **repo refresh**
work without one (they just operate on the built-in default repository alone).

| Operation | Behavior |
|-----------|----------|
| **Repo list** | Show every configured repository with its resolved metadata and status tags (`[default]`, `[disabled]`, `[error: …]`). Works in-memory. |
| **Repo add** | Register a user repository by its `logos-repo.json` URL. Resolves metadata **immediately** so a bad URL is reported now, not at first browse. Persisted. Requires a config file. |
| **Repo remove** | Drop a user repository. The default repository cannot be removed. Persisted. Requires a config file. |
| **Repo enable / disable** | Toggle a repository's participation in the merged catalog. Disabling the default sets the `defaultDisabled` flag. Persisted. Requires a config file. |
| **Repo refresh** | Clear caches and re-fetch every repository's `logos-repo.json` and `index.json`. Reports per-repository fetch errors but does not fail overall. Works in-memory. |

### Configuration operations

| Operation | Behavior |
|-----------|----------|
| **Config init** | Create an empty config file (`schemaVersion 1`, no repositories, default enabled). |
| **Config show** | Print the raw config file contents. |
| **Config path** | Print the active config path, or `(none)` when running in-memory. |

### Programmatic capabilities

Beyond the above, the library/C boundary additionally offers:

- **Merged catalog as JSON** and **single-repository catalog as JSON**, in the
  synthesised shape (`repositoryUrl`/`repositoryName`/`repositoryDisplayName`,
  package header fields, date-sorted `versions[]`).
- **Repository list as JSON**, with resolved metadata and `resolveError` per
  entry.
- **Dependency resolution as JSON**, with the optional installed-state
  snapshot.
- **A standalone range matcher**, for callers that want to filter versions
  without running the full resolver.
- **A pluggable fetcher**, so tests and embedders can supply their own HTTP
  transport instead of the default network client.

---

## Use Cases & Workflows

### Browse the catalog (zero setup)

The default repository is built in, so browsing works with no config:

```
lgpd list
lgpd search waku
lgpd info wallet_module --json
```

### Add and manage a custom repository

```
lgpd config init ~/.config/logos/repositories.json
lgpd --config ~/.config/logos/repositories.json repo add https://example.com/my/logos-repo.json
lgpd --config ~/.config/logos/repositories.json repo list
lgpd --config ~/.config/logos/repositories.json repo refresh
```

`repo add` resolves the new repository's metadata on the spot — a typo'd or
unreachable URL fails the command immediately rather than silently registering
a dead source.

### Download a verified package, then install it

```
lgpd download wallet_module --version 1.0.0 --repo my-catalog -o ./packages/
#  → fetches the .lgx, verifies rootHash + manifest + signer against the
#    catalog, prints the local path
#  → hand that path to lgpm to install (trust-checked)
```

### Plan an install across repositories (programmatic)

A higher-level installer constructs the library, asks it to resolve a
dependency graph — optionally informed by what is already on disk — then
downloads each resolved version before delegating the actual install to the
package manager:

```
   resolveDependencies( wanted, installedSnapshot )
       → ordered list of { name, version, rootHash, url, repositoryUrl, topLevel }
   for each entry:  downloadPackage(...)        (skip already-installed transitives)
   hand the .lgx files to the package manager
```

### Embed inside the platform (in-process module)

The in-platform package-downloader module drives the same logic through the
C boundary: create a context (optionally backed by a config file), add
repositories, fetch the catalog, resolve dependencies, and download — all
returning JSON strings the module forwards to the rest of the platform over
Qt Remote Objects.

---

## Observable Behavior & Contracts

### CLI exit semantics

| Outcome | Exit code |
|---------|-----------|
| Success | `0` |
| Help (`-h` / `--help`) and version (`-V` / bare `--version`) | `0` |
| Missing required argument (e.g. `search` with no query, `info`/`download` with no package) | `1` |
| Package not found (`info`), download failed/verification rejected (`download`) | `1` |
| Repository mutation without `--config`, or `config show` without `--config` | `1` |
| Unknown command or subcommand | `1` |
| No positional command at all | `1` (prints help) |

The `--version` flag is overloaded: `--version <value>` **pins a package
version** for `download`/`info`, while a bare `--version` with nothing following
it prints the tool version. `-V` always prints the tool version.

### Output modes

Every catalog/repository operation supports a `--json` mode that emits the
structured JSON shape directly; without it, human-readable tables and labelled
fields are printed. This makes the same command usable interactively and as a
machine-readable data source for scripts and agents.

### Guarantees

- **No partial artifacts.** A failed transfer or a failed verification never
  leaves a `.lgx` on disk for a later step to mistake for a good download.
- **Verification is mandatory.** A download that cannot be bound to its catalog
  entry is rejected; the path is only returned for a verified file.
- **Federation is fault-tolerant.** A broken repository degrades to an empty
  contribution and a recorded error, never a global failure.
- **In-memory safety.** Without a config file, browsing and downloading still
  work against the default repository; only persistence-requiring repository
  mutations are refused with a clear message.
- **Same shapes everywhere.** The CLI, the library, and the C boundary all
  produce the identical JSON for catalogs, repository lists, and resolution
  results.

---

## Constraints & Limitations

- **HTTPS only.** A non-`https` repository URL is rejected
  (`unsupported URL scheme (https required in v1)`). The downloaded `.lgx`
  `url` is followed wherever it points (including redirects).
- **Repository mutations need a config file.** `add` / `remove` / `enable` /
  `disable` and `config show` require a config path; an in-memory client
  refuses them.
- **The default repository URL is compiled in.** It can be disabled or
  removed by the user; re-adding the same URL restores it.
- **The range matcher is a semver subset.** Build metadata is ignored; only
  basic pre-release ordering is honoured.
- **Trust is not evaluated here.** Verification binds index → file (same DID,
  same content, same manifest). Deciding whether the signer is *trusted* is the
  package manager's responsibility at install time.
- **Metadata is resolved once per process.** A stale view is corrected only by
  an explicit refresh.
- **Legacy catalog rows are tolerated.** Versions with a null manifest or a
  missing `rootHash` skip the corresponding verification facet rather than
  being rejected.

---

## Relationship to Other Components

| Component | Relationship |
|-----------|--------------|
| **logos-package-manager (`lgpm`)** | Downstream peer. The downloader produces the verified `.lgx`; the package manager installs it (and does the deep Ed25519 trust check). Together they are the platform's package pipeline. |
| **logos-package (`lgx`)** | Supplies the package format and the verification primitives the downloader uses to confirm structure, read the manifest, and read the signer of a downloaded `.lgx`. |
| **logos-modules-release-tool / logos-modules-release-base** | Define and produce the `logos-repo.json` + `index.json` catalog formats the downloader consumes. The downloader is a read-only client of those formats. |
| **package-downloader Logos module** | Wraps this library behind the C boundary so the rest of the Logos platform can browse and download over its standard inter-module RPC. |
