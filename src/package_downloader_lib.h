#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lgpd {

/// Default repository URL (points to logos-co's logos-repo.json). Treated as
/// an opaque string by the rest of the code: the only reason it is wired up
/// at compile time is so the client always has *something* to talk to, even
/// when no config file is provided.
extern const char* kDefaultRepositoryUrl;

struct FetchResult {
    bool ok = false;
    std::string error;
};

/// Byte-progress sink for one transfer. `total` is 0 for "size unknown" —
/// never divide by it.
using ProgressFn = std::function<void(std::uint64_t received, std::uint64_t total)>;

/// Minimal HTTP(S) fetcher abstraction. The concrete implementation in the
/// .cpp uses libcurl. Tests can inject their own implementation.
class Fetcher {
public:
    virtual ~Fetcher() = default;

    /// HTTP GET. Succeeds on 2xx with the response body in `out`.
    virtual FetchResult get(const std::string& url, std::string& out) = 0;

    /// HTTP GET to a file. Succeeds on 2xx after writing all bytes.
    virtual FetchResult getToFile(const std::string& url, const std::string& path) = 0;

    /// Same, reporting byte progress as the body streams in. Defaulted to
    /// the progress-free overload so existing implementations still compile
    /// and simply report nothing.
    virtual FetchResult getToFile(const std::string& url,
                                  const std::string& path,
                                  const ProgressFn& onProgress) {
        (void)onProgress;
        return getToFile(url, path);
    }
};

/// One repository entry in the in-memory registry. The persisted form is
/// just `{ url, enabled }`; everything else is fetched at runtime from
/// `logos-repo.json`.
struct Repository {
    std::string url;          ///< URL of `logos-repo.json`
    bool enabled = true;
    bool isDefault = false;   ///< Synthesised at runtime, never persisted.

    // Resolved at runtime from logos-repo.json. Empty when fetch failed.
    std::string name;         ///< canonical id
    std::string displayName;
    std::string description;
    std::string homepage;
    std::string indexUrl;
    /// `trustedSigners[].did` as ADVERTISED by the repository's own
    /// logos-repo.json. ADVISORY ONLY — parsed, stored, and echoed back in
    /// listRepositoriesJson(); consulted by nothing, deliberately.
    ///
    /// This is a repository's self-assertion about itself, and a downloaded
    /// claim establishes no trust anchor. Wiring it into an install decision
    /// would let a repository authorise its own packages, which is exactly the
    /// failure the name invites. The only anchor set is the LOCAL keyring
    /// (`lgx keyring`, and logos-package-manager's addTrustedKey /
    /// removeTrustedKey / listTrustedKeys), which nothing enters except by an
    /// explicit user act.
    ///
    /// Legitimate future use: an "this repository publishes as X — add to your
    /// keyring?" affordance in the UI, behind a user action that calls
    /// addTrustedKey. Never an auto-promotion, and never read by the installer.
    /// (The official catalog ships `"trustedSigners": []` today, so it vouches
    /// for nobody in any case.)
    std::vector<std::string> trustedSignerDids;

    /// Per-module link TEMPLATES the repository declares, with `{name}` and
    /// `{version}` placeholders expanded per catalog row.
    ///
    /// `reportUrlTemplate` is where a user reports a module that misbehaves
    /// (App Store guideline 4.7.1); `universalLinkTemplate` is the link that
    /// addresses one module from outside the app (4.7.4). A Store shell needs
    /// both per module, and a catalog with three hundred modules should not have
    /// to repeat one URL three hundred times -- so the repository declares the
    /// shape and the client expands it. A package may override either with its
    /// own explicit `reportUrl` / `universalLink`.
    ///
    /// Empty when logos-repo.json declares none; the catalog row then carries
    /// empty strings rather than omitting the keys.
    std::string reportUrlTemplate;
    std::string universalLinkTemplate;

    std::string resolveError; ///< non-empty when the fetch / parse failed
};

/// Registry of repositories. Persists `{ url, enabled }` plus a
/// `defaultDisabled` flag. When enabled, the default repo is listed first;
/// when disabled it is omitted from `list()` entirely. Only user repos are
/// written into `repositories[]`.
class RepositoryRegistry {
public:
    /// Construct an in-memory registry with no config-file backing. Mutating
    /// methods (add/remove/setEnabled) will return an error. The default
    /// repo is present (enabled).
    RepositoryRegistry();

    /// Construct backed by a JSON config file. The file is loaded if it
    /// exists. Missing parent dirs are created lazily on save.
    explicit RepositoryRegistry(std::string configPath);

    ~RepositoryRegistry();

    /// Set the fetcher used to populate resolved metadata. Defaults to a
    /// libcurl-backed `HttpsFetcher`.
    void setFetcher(std::shared_ptr<Fetcher> fetcher);

    /// Returns the in-memory list, default-first (when not disabled) then
    /// user repos in declared order. Each entry has its `enabled` flag and
    /// its resolved metadata (if `refresh()` has been called and the fetch
    /// succeeded).
    std::vector<Repository> list() const;

    /// Add a user repo by URL, or re-enable the default repo when `url` is
    /// `kDefaultRepositoryUrl`. The URL must point to a `logos-repo.json`
    /// (or wherever the client can fetch one). On success persists the
    /// updated config to disk. Returns an empty string on success, or an
    /// error message.
    std::string addRepository(const std::string& url);

    /// Remove a user repo by URL, or permanently disable the default repo
    /// (sets `defaultDisabled`; `list()` omits it). Persists on success.
    std::string removeRepository(const std::string& url);

    /// Enable or disable a repo. Allowed for any entry. Toggling the
    /// default sets the `defaultDisabled` flag in the config file (same
    /// omit-from-list semantics as `removeRepository` on the default).
    std::string setEnabled(const std::string& url, bool enabled);

    /// Re-fetch `logos-repo.json` for every enabled repo (skips a disabled
    /// default). After calling, `list()` returns entries with resolved
    /// metadata fields populated. Returns an empty string on overall
    /// success or a summary of fetch errors (the registry is best-effort:
    /// failures are recorded per-entry in `resolveError` but do not abort).
    std::string refresh();

    /// Look up a repo by URL or by canonical name. Returns nullopt if no
    /// match.
    std::optional<Repository> findByUrlOrName(const std::string& urlOrName) const;

    /// Whether the registry was constructed with a config-file path.
    bool isPersistent() const;

    /// Path to the backing config file, or empty when in-memory only.
    std::string configPath() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// High-level operations on top of `RepositoryRegistry`.
///
/// The public API speaks JSON strings rather than typed structs so it is
/// trivially callable from a C wrapper, the package-downloader Logos
/// module, and tests. The JSON shapes mirror the schemas documented in
/// the plan (`index.json#packages[]` + a few synthesised fields).
class PackageDownloaderLib {
public:
    PackageDownloaderLib();
    explicit PackageDownloaderLib(std::string configPath);
    ~PackageDownloaderLib();

    PackageDownloaderLib(const PackageDownloaderLib&) = delete;
    PackageDownloaderLib& operator=(const PackageDownloaderLib&) = delete;

    void setFetcher(std::shared_ptr<Fetcher> fetcher);

    /// Returns the registry (mutable).
    RepositoryRegistry& registry();
    const RepositoryRegistry& registry() const;

    /// JSON array of repositories. Each element:
    /// `{ url, enabled, isDefault, name, displayName, description,
    ///    homepage, indexUrl, trustedSignerDids[], reportUrlTemplate,
    ///    universalLinkTemplate, resolveError }`.
    std::string listRepositoriesJson();

    /// JSON array of all packages across all enabled repos. Each element:
    /// `{ repositoryUrl, repositoryName, name, variants: [...], versions: [...] }`.
    /// `versions[]` is sorted newest-first by `releasedAt` and contains
    /// `releasedAt, publisherRef, url, size, sha256, rootHash, manifest,
    /// signature?` exactly as in `index.json`.
    /// `variants[]` is the sorted platform-variant list of the NEWEST version
    /// (the keys of its manifest `main`) — what a caller filters on to answer
    /// "will this run on my device" before downloading anything. Always
    /// present, empty when the newest version declares none.
    /// `reportUrl` and `universalLink` are the per-module links a Store shell
    /// needs (guideline 4.7.1 / 4.7.4): the package's own values if it declares
    /// them, else the repository's template with `{name}` / `{version}`
    /// expanded, else empty. Always present, like `variants`.
    std::string getCatalogJson();

    /// JSON array of all packages for one repo (URL or canonical name).
    /// Same shape as `getCatalogJson()` filtered to one source.
    std::string getCatalogForRepoJson(const std::string& urlOrName);

    /// Force re-fetch of every enabled repo's `index.json`. Returns an
    /// empty string on success or a summary of errors.
    std::string refreshCatalogs();

    /// Download a package. If `rootHash` is empty and multiple entries
    /// share the same `version`, pick the newest by `releasedAt`. If
    /// `version` is empty, pick the newest version. `repoUrlOrName` may be
    /// empty to mean "any enabled repo, in registry order".
    /// Returns the local path to the downloaded `.lgx`, or empty on error
    /// with the reason in `errorMessage`.
    ///
    /// An empty `outputDir` stages into a private per-user directory under
    /// the system temp dir, NOT the shared temp root: the published filename
    /// is derived from the package and version, so two accounts on one host
    /// would otherwise collide on a path neither can take from the other.
    ///
    /// `onProgress` runs on the calling thread, already rate-limited. `total`
    /// is the transport's Content-Length, else the catalog's advertised size,
    /// else 0. It covers the TRANSFER ONLY — index-binding verification runs
    /// after the last callback, so 100% is not "done".
    std::string downloadPackage(const std::string& repoUrlOrName,
                                const std::string& packageName,
                                std::string& errorMessage,
                                const std::string& version = "",
                                const std::string& rootHash = "",
                                const std::string& outputDir = "",
                                const ProgressFn& onProgress = {});

    /// Cross-repo dependency resolution.
    ///
    /// Given a starting package's dependency list (one element per
    /// dependency, in the Dependency JSON form described by the manifest
    /// schema), returns a JSON array of resolved versions:
    ///   `[{ repositoryUrl, name, version, rootHash, url, size, topLevel }, ...]`
    /// in install order (deps before dependents, no duplicates).
    /// `size` is the advertised `.lgx` size in bytes (0 if the index omits
    /// it), so a caller can total a whole plan before any bytes move.
    /// `topLevel: true` marks entries that came from the input array (the
    /// packages the caller explicitly requested); other entries are
    /// transitive deps the resolver pulled in.
    ///
    /// `installedPackagesJson` is an optional `[{ name, version, rootHash }, ...]`
    /// describing what's currently on disk. When supplied, the resolver
    /// uses it to short-circuit TRANSITIVE deps that are already
    /// satisfied: if an installed copy's version meets the dep's
    /// range, that dep is omitted from the output entirely (no install
    /// or change needed). Top-level entries (from the input array) are
    /// always resolved against the catalog — the caller picked them
    /// explicitly. Empty/missing installedPackagesJson disables the
    /// short-circuit and reproduces the pre-installed-aware behaviour
    /// (every transitive dep resolves to a catalog pick).
    ///
    /// When a constraint cannot be satisfied, an entry of the form
    /// `{ error: "...", name: "..." }` is included at the unsatisfied
    /// position and resolution stops; callers should check for `error`.
    std::string resolveDependenciesJson(const std::string& dependenciesJson,
                                        const std::string& installedPackagesJson = "");

    /// True if the given semver range matches the given concrete version.
    /// Exposed for tests and for callers that want to filter without going
    /// through the full resolver.
    static bool semverMatches(const std::string& range, const std::string& version);

    /// Does a candidate whose catalog signature carries `candidateSignerDid`
    /// satisfy a dependency's `signer` pin?
    ///
    /// SELECTION, not authorization. A signer pin disambiguates among
    /// same-named candidates; satisfying it says nothing about whether the
    /// package may be installed, which is decided later and elsewhere by the
    /// trust-anchor policy in logos-package-manager. Keep the two apart.
    ///
    /// An empty pin never matches anything, and a candidate with no signature
    /// DID is never matched by anything. The predicate used to be a bare
    /// inequality, which made `"signer": ""` select exactly the UNSIGNED
    /// releases of a package rather than mean "unpinned".
    ///
    /// Exposed so the invariant can be tested without standing up a catalog.
    static bool signerPinMatches(const std::string& pin,
                                 const std::string& candidateSignerDid);

    /// Does a DOWNLOADED FILE satisfy the signer the catalog advertised for
    /// it? A different question from signerPinMatches, on a different input:
    /// that one compares two pieces of CATALOG metadata while choosing what to
    /// fetch; this one compares the catalog against the bytes that arrived.
    ///
    /// All three facts must line up: the file is signed, its signature
    /// VERIFIES, and the DID it verifies under is the advertised one.
    ///
    /// `fileSignatureValid` is the half that was missing. logos-package
    /// populates `signer_did` out of manifest.sig BEFORE running the Ed25519
    /// check, so a DID alone is a CLAIM — and the advertised DID is public
    /// catalog data. Without the validity term, substituting a package that
    /// merely NAMES the advertised DID in a hand-written manifest.sig passed
    /// the binding, no key required.
    ///
    /// Still not authorization: no keyring is consulted here, and none should
    /// be. This asks only whether the bytes we received are the ones the
    /// advertised publisher actually signed.
    ///
    /// Exposed, like signerPinMatches, so the invariant can be tested next to
    /// the comparison it constrains rather than only through a live download.
    static bool downloadedSignerBinds(bool fileSigned,
                                      bool fileSignatureValid,
                                      const std::string& fileSignerDid,
                                      const std::string& advertisedDid);

    /// The resolver's ranking rule: does the candidate outrank the incumbent?
    ///
    /// SemVer precedence decides; `releasedAt` only breaks ties between equal
    /// versions. The resolver used to rank on `releasedAt` alone, so a 1.2.x
    /// hotfix backported after 2.0.0 shipped — newer timestamp, older version —
    /// would be chosen over 2.0.0.
    ///
    /// Exposed so the rule can be tested without standing up a live catalog.
    static bool outranks(const std::string& candidateVersion, const std::string& candidateDate,
                         const std::string& incumbentVersion, const std::string& incumbentDate);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lgpd

// ─────────────────────────────────────────────────────────────────────────────
// Backwards-compat C++ alias.
// ─────────────────────────────────────────────────────────────────────────────
// The historical public class name was `::PackageDownloaderLib`. The Logos
// module wrapper and existing CLI code use that. We re-export it here so the
// rewrite can land without breaking compile of every consumer in the same
// commit. New code should use `lgpd::PackageDownloaderLib`.
using ::lgpd::PackageDownloaderLib;
