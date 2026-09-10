// Logos package downloader — multi-repository client.
//
// This file implements:
//   • `lgpd::RepositoryRegistry` — manages the hardcoded default repo plus
//     a user-managed list persisted as JSON. Fetches `logos-repo.json`
//     from each entry at refresh time to resolve canonical metadata.
//   • `lgpd::PackageDownloaderLib` — merges every enabled repo's
//     `index.json` into one catalog, downloads `.lgx` files by their URL,
//     and performs semver-aware cross-repo dependency resolution.
//
// The `logos-repo.json` and `index.json` shapes consumed here are
// specified in logos-modules-release-tool's docs/catalog-format.md:
// https://github.com/logos-co/logos-modules-release-tool/blob/main/docs/catalog-format.md
// The client-side repository config (`{ defaultDisabled, repositories:
// [{ url, enabled }] }`) is described in RepositoryRegistry::Impl below.

#include "package_downloader_lib.h"
#include "progress_throttle.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <lgx.h>   // lgx_load / lgx_get_manifest_json / lgx_verify_signature
#include <logos/semver.hpp>   // the shared semver implementation (also from logos-package)

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace lgpd {

const char* kDefaultRepositoryUrl =
    "https://raw.githubusercontent.com/logos-co/logos-modules-release/refs/heads/main/logos-repo.json";

namespace {

// ─── Small string utilities ──────────────────────────────────────────────────

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

// nlohmann::json::value(key, default) returns `default` only when the key is
// absent; if the key is present with a `null` value, value() returns the null
// itself and any chained .value() call on it throws json::type_error 306
// ("cannot use value() with null"). The catalog has `manifest: null` rows in
// practice (an `index.json` produced by an early action run), so chains like
//   v.value("manifest", json::object()).value("version", "")
// will crash on those rows.
//
// objOrEmpty returns a reference to either the child JSON object or a static
// empty object — never a null. Use it at the top of any chain that descends
// into an optional nested object.
const json& objOrEmpty(const json& parent, const char* key) {
    static const json kEmpty = json::object();
    if (!parent.is_object()) return kEmpty;
    auto it = parent.find(key);
    if (it == parent.end() || !it->is_object()) return kEmpty;
    return *it;
}

// The platform variants a manifest ships, as a sorted array of names — the
// keys of its `main` map. The vocabulary is logos-package's (desktop, mobile
// and web, see its docs/spec.md); a catalog only reports what it is told, so
// this neither validates nor canonicalises. Sorted so a row is stable across
// index rebuilds, which is what makes two catalog fetches diffable.
//
// Anything that is not a variant map — `main`'s plain-string form, an index
// row with no manifest — yields an empty array rather than nothing at all.
json variantsOf(const json& manifest) {
    std::vector<std::string> names;
    const json& main = objOrEmpty(manifest, "main");
    for (auto it = main.begin(); it != main.end(); ++it) names.push_back(it.key());
    std::sort(names.begin(), names.end());
    return json(names);
}

// ─── Semver ───────────────────────────────────────────────────────────────────
//
// Parsing, precedence and range matching all come from the shared
// implementation in logos-package (include/logos/semver.hpp) — the same code
// lgx, lgpm and the package-manager UI now use.
//
// What used to live here was a hand-rolled parser that compared the entire
// pre-release tag as one ASCII string, so `1.0.0-rc.11` sorted BELOW
// `1.0.0-rc.2`; and its ranges had no pre-release rule at all, so `^1.0.0`
// matched `2.0.0-alpha` — an unreleased alpha of the next major satisfying a
// caret range on 1.x.

bool semverRangeMatches(const std::string& range, const std::string& version) {
    return logos::semver::satisfies(version, range);
}

// ─── libcurl fetcher ──────────────────────────────────────────────────────────

class CurlGlobalInit {
public:
    CurlGlobalInit()  { curl_global_init(CURL_GLOBAL_DEFAULT); }
    ~CurlGlobalInit() { curl_global_cleanup(); }
};

CurlGlobalInit& curlInit() {
    static CurlGlobalInit g;
    return g;
}

size_t curlWriteMem(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* out = static_cast<std::string*>(userp);
    out->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

size_t curlWriteFile(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* file = static_cast<std::ofstream*>(userp);
    file->write(static_cast<const char*>(contents),
                static_cast<std::streamsize>(size * nmemb));
    return file->good() ? size * nmemb : 0;
}

// Monotonic ms for rate limiting. Steady, not system, clock: a wall-clock
// adjustment mid-download must not stall or spam progress.
std::uint64_t monotonicNowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// libcurl progress trampoline; `userp` is the caller's ProgressFn. Always
// returns 0 — non-zero would ABORT the transfer, and a slow sink must not be
// able to fail a good download.
int curlXferInfo(void* userp, curl_off_t dltotal, curl_off_t dlnow,
                 curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    const auto& fn = *static_cast<const ProgressFn*>(userp);
    if (fn && dlnow >= 0 && dltotal >= 0) {
        // This is a C boundary: letting an exception unwind through libcurl's
        // frames is undefined behaviour. A sink that throws — bad_alloc while
        // marshalling an event, say — must cost a progress sample, not the
        // process.
        try {
            fn(static_cast<std::uint64_t>(dlnow), static_cast<std::uint64_t>(dltotal));
        } catch (...) {
        }
    }
    return 0;
}

// Append a unique cache-busting query param.
//
// The metadata we GET (logos-repo.json from raw.githubusercontent.com,
// index.json from a GitHub release asset) is mutable but sits behind
// Fastly. After a catalog rebuild the edge can keep serving the OLD
// body for a window even with `Cache-Control: no-cache` (that means
// "revalidate", and the release-download 302 itself is edge-cached).
// A unique query string changes the cache key → guaranteed origin
// fetch → the client never shows a removed/renamed module just
// because it reloaded too soon after an upstream change. Unbounded
// freshness matters more than CDN offload for these tiny JSON files;
// the actual `.lgx` blobs (getToFile) are immutable per version and
// are intentionally left cacheable.
std::string cacheBustedUrl(const std::string& url) {
    static std::atomic<unsigned long long> seq{0};
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    const unsigned long long n = seq.fetch_add(1, std::memory_order_relaxed);
    const char sep = (url.find('?') == std::string::npos) ? '?' : '&';
    return url + sep + "_lgpd_cb=" +
           std::to_string(static_cast<unsigned long long>(ns)) + "-" +
           std::to_string(n);
}

// Point libcurl at a CA-certificate bundle from the host system.
//
// nixpkgs builds curl against OpenSSL whose only built-in trust anchor is
// a /nix store path (ultimately /nix/var/nix/profiles/default/etc/ssl/
// certs/ca-bundle.crt) that exists ONLY when Nix is installed. A portable
// build copied to a machine without Nix therefore has no trust store, and
// every HTTPS fetch fails certificate verification (CURLE_SSL_CACERT) —
// which is exactly why installing Nix "fixes" the download with no rebuild.
// To stay self-contained we honor the standard env overrides first, then
// probe the well-known per-distro/OS CA locations and pin them explicitly.
// Resolved once and shared by every transfer (the lgpd CLI and the
// in-process package_downloader module both reach here).
void applyCaBundle(CURL* c) {
#ifdef _WIN32
    // Windows has no filesystem CA bundle to probe for: trust anchors live in
    // the system certificate store, reachable only through the Win32 crypto
    // API. Every path below would miss, leaving curl on its compiled-in
    // default -- which for a Nix-cross-built libcurl is a /nix/store path that
    // does not exist on the target machine, so every HTTPS fetch fails with an
    // opaque "fetch failed".
    //
    // CURLSSLOPT_NATIVE_CA (curl >= 7.71) makes the OpenSSL backend import the
    // Windows store instead. Set rather than probed, so it is also correct on
    // a machine whose certificates were updated after this binary was built.
    curl_easy_setopt(c, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
    return;
#else
    // { CAINFO file, CAPATH dir }. Both empty ⇒ nothing usable found, so
    // leave curl on its compiled-in default rather than break a working host.
    static const std::pair<std::string, std::string> ca = [] {
        auto fileOk = [](const char* p) {
            std::error_code ec; return p && *p && fs::is_regular_file(p, ec);
        };
        auto dirOk = [](const char* p) {
            std::error_code ec; return p && *p && fs::is_directory(p, ec);
        };
        // 1) Explicit env overrides. libcurl/OpenSSL honor some of these
        //    natively; setting them ourselves keeps behavior identical across
        //    SSL backends. Guarded by existence so a stale path (e.g. an
        //    inherited NIX_SSL_CERT_FILE pointing at a vanished store path)
        //    doesn't shadow the system probe below.
        for (const char* var : {"CURL_CA_BUNDLE", "SSL_CERT_FILE", "NIX_SSL_CERT_FILE"}) {
            const char* v = std::getenv(var);
            if (fileOk(v)) return std::make_pair(std::string(v), std::string());
        }
        if (const char* d = std::getenv("SSL_CERT_DIR"); dirOk(d))
            return std::make_pair(std::string(), std::string(d));
        // 2) Well-known system CA bundle files (first existing wins).
        for (const char* p : {
                 "/etc/ssl/certs/ca-certificates.crt",                 // Debian/Ubuntu/Arch/Gentoo, NixOS
                 "/etc/pki/tls/certs/ca-bundle.crt",                   // Fedora/RHEL/CentOS
                 "/etc/ssl/ca-bundle.pem",                             // openSUSE
                 "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",  // CentOS/RHEL 7+
                 "/etc/ssl/cert.pem",                                  // macOS, Alpine, FreeBSD
             }) {
            if (fileOk(p)) return std::make_pair(std::string(p), std::string());
        }
        // 3) Well-known hashed-symlink CA directories.
        for (const char* p : {"/etc/ssl/certs", "/etc/pki/tls/certs"}) {
            if (dirOk(p)) return std::make_pair(std::string(), std::string(p));
        }
        return std::make_pair(std::string(), std::string());
    }();

    if (!ca.first.empty())  curl_easy_setopt(c, CURLOPT_CAINFO, ca.first.c_str());
    if (!ca.second.empty()) curl_easy_setopt(c, CURLOPT_CAPATH, ca.second.c_str());
#endif  // _WIN32
}

// Compact reason a libcurl transfer failed: the libcurl error string (with
// its numeric code) when the request never completed, else the non-2xx HTTP
// status. Carried in FetchResult::error so an opaque "fetch failed"
// becomes diagnosable — e.g. a host with no usable CA bundle reports
// "SSL peer certificate or SSH remote key was not OK" instead of nothing.
std::string curlFailDetail(CURLcode res, long httpCode) {
    if (res != CURLE_OK)
        return std::string(curl_easy_strerror(res)) +
               " (curl error " + std::to_string(static_cast<int>(res)) + ")";
    return "HTTP status " + std::to_string(httpCode);
}

class HttpsFetcher : public Fetcher {
public:
    FetchResult get(const std::string& url, std::string& out) override {
        curlInit();
        CURL* c = curl_easy_init();
        if (!c) return {false, "curl_easy_init failed"};
        applyCaBundle(c);
        out.clear();
        // Cache-buster + no-cache headers so a reload right after an
        // upstream catalog change never re-serves a stale edge copy.
        const std::string busted = cacheBustedUrl(url);
        struct curl_slist* hdrs = nullptr;
        hdrs = curl_slist_append(hdrs, "Cache-Control: no-cache, no-store, max-age=0");
        hdrs = curl_slist_append(hdrs, "Pragma: no-cache");
        curl_easy_setopt(c, CURLOPT_URL, busted.c_str());
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlWriteMem);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "lgpd/2.0");
        CURLcode res = curl_easy_perform(c);
        long code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(c);
        if (res != CURLE_OK || code < 200 || code >= 300)
            return {false, curlFailDetail(res, code)};
        return {true, {}};
    }

    FetchResult getToFile(const std::string& url, const std::string& path) override {
        return getToFile(url, path, ProgressFn{});
    }

    FetchResult getToFile(const std::string& url, const std::string& path,
                          const ProgressFn& onProgress) override {
        curlInit();
        std::error_code ec;
        fs::create_directories(fs::path(path).parent_path(), ec);
        std::ofstream f(path, std::ios::binary);
        if (!f.is_open()) {
            return {false, "cannot open " + path + " for writing"};
        }

        // Remove the destination on any failure so a half-written or
        // empty file is never left behind for a later step (or the
        // user) to mistake for a good download. The body may already
        // have been partially streamed to disk by curlWriteFile before
        // a non-2xx status or transfer error is known.
        auto fail = [&](std::string why) -> FetchResult {
            f.close();
            std::error_code rmEc;
            fs::remove(path, rmEc);
            return {false, std::move(why)};
        };

        CURL* c = curl_easy_init();
        if (!c) {
            return fail("curl_easy_init failed");
        }

        applyCaBundle(c);
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlWriteFile);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &f);
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_TIMEOUT, 600L);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "lgpd/2.0");
        // Reports the FINAL response's size, after FOLLOWLOCATION has chased
        // redirects — needed for GitHub release assets, where only the
        // redirect target carries a Content-Length. `dltotal` is 0 until the
        // headers land, and stays 0 for a chunked response.
        if (onProgress) {
            curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, curlXferInfo);
            curl_easy_setopt(c, CURLOPT_XFERINFODATA, &onProgress);
            curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        }
        CURLcode res = curl_easy_perform(c);
        long code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_cleanup(c);
        f.close();

        if (res != CURLE_OK || code < 200 || code >= 300) {
            return fail(curlFailDetail(res, code));
        }

        return {true, {}};
    }
};

// ─── logos-repo.json + index.json parsers ─────────────────────────────────────

bool parseLogosRepoJson(const std::string& body, Repository& dst, std::string& err) {
    try {
        auto j = json::parse(body);
        if (!j.is_object()) { err = "logos-repo.json is not a JSON object"; return false; }
        auto must = [&](const char* k, bool isString = true) {
            if (!j.contains(k)) { err = std::string("missing field '") + k + "'"; return false; }
            if (isString && !j[k].is_string()) { err = std::string("field '") + k + "' is not a string"; return false; }
            return true;
        };
        if (!must("name")) return false;
        if (!must("displayName")) return false;
        if (!must("indexUrl")) return false;
        dst.name        = j["name"].get<std::string>();
        dst.displayName = j["displayName"].get<std::string>();
        dst.description = j.value("description", "");
        dst.homepage    = j.value("homepage", "");
        dst.indexUrl    = j["indexUrl"].get<std::string>();
        dst.trustedSignerDids.clear();
        if (j.contains("trustedSigners") && j["trustedSigners"].is_array()) {
            for (const auto& s : j["trustedSigners"]) {
                if (s.is_object() && s.contains("did") && s["did"].is_string()) {
                    dst.trustedSignerDids.push_back(s["did"].get<std::string>());
                }
            }
        }
        return true;
    } catch (const json::exception& e) {
        err = std::string("JSON parse error: ") + e.what();
        return false;
    }
}

bool isHttpsUrl(const std::string& url) {
    return url.rfind("https://", 0) == 0;
}

// Order `index.json#packages[].versions[]` newest-first, by SemVer precedence.
//
// This is the ordering every client ultimately sees: getCatalogJson re-sorts on
// read, so it OVERRIDES whatever order the catalog file was written in, and the
// package-manager UI reads versions[0] as "latest".
//
// It used to sort on `releasedAt` alone, which is a publish time, not a version:
// a `2.0.0-alpha` cut after `1.9.0`, or a `1.2.1` backported after `2.0.0`,
// took the top slot and was shown to users as the newest release. The date now
// only breaks ties between entries sharing a version (the same version
// republished with a different rootHash).
//
// Null entries in versions[] would crash value() with type_error 306 — fall
// back to empty strings, which sort last (an unparseable version ranks below
// every real one).
struct VersionPrecedenceDesc {
    bool operator()(const json& a, const json& b) const {
        const auto versionOf = [](const json& v) {
            return v.is_object() ? objOrEmpty(v, "manifest").value("version", "") : std::string();
        };
        const auto dateOf = [](const json& v) {
            return v.is_object() ? v.value("releasedAt", "") : std::string();
        };
        const int cmp = logos::semver::compare(versionOf(a), versionOf(b));
        if (cmp != 0) return cmp > 0;
        return dateOf(a) > dateOf(b);
    }
};

} // namespace

// ─── RepositoryRegistry::Impl ─────────────────────────────────────────────────

struct RepositoryRegistry::Impl {
    std::string configPath;
    bool persistent = false;
    // Two independent flags for the default repo:
    //   defaultDisabled — soft state; row stays in list() with enabled=false
    //                     so Settings can render a toggled-off entry.
    //   defaultRemoved  — hard state; list() omits the default entirely.
    //                     User must re-add by URL. Independent so a re-add
    //                     restores the disabled state deterministically.
    bool defaultDisabled = false;
    bool defaultRemoved  = false;
    std::vector<Repository> userRepos;        // persisted (url + enabled only)
    Repository defaultRepo;                   // always present
    std::shared_ptr<Fetcher> fetcher;
    mutable std::mutex mu;

    Impl() {
        defaultRepo.url = kDefaultRepositoryUrl;
        defaultRepo.isDefault = true;
        defaultRepo.enabled = true;
        fetcher = std::make_shared<HttpsFetcher>();
    }

    void load() {
        if (!persistent) return;
        std::ifstream f(configPath);
        if (!f.is_open()) return;
        try {
            json j; f >> j;
            defaultDisabled = j.value("defaultDisabled", false);
            defaultRemoved  = j.value("defaultRemoved",  false);
            userRepos.clear();
            if (j.contains("repositories") && j["repositories"].is_array()) {
                for (const auto& r : j["repositories"]) {
                    if (!r.is_object() || !r.contains("url") || !r["url"].is_string()) continue;
                    Repository repo;
                    repo.url = r["url"].get<std::string>();
                    repo.enabled = r.value("enabled", true);
                    userRepos.push_back(std::move(repo));
                }
            }
        } catch (...) {
            // Ignore parse errors; treat as empty config.
        }
    }

    std::string save() {
        if (!persistent) return "registry is in-memory only (pass --config <path>)";
        json j;
        j["schemaVersion"] = 1;
        j["defaultDisabled"] = defaultDisabled;
        j["defaultRemoved"]  = defaultRemoved;
        json arr = json::array();
        for (const auto& r : userRepos) {
            json e;
            e["url"] = r.url;
            e["enabled"] = r.enabled;
            arr.push_back(std::move(e));
        }
        j["repositories"] = std::move(arr);
        std::error_code ec;
        fs::create_directories(fs::path(configPath).parent_path(), ec);
        std::ofstream f(configPath);
        if (!f.is_open()) return "cannot write config file: " + configPath;
        f << j.dump(2);
        return f.good() ? std::string() : ("write failed: " + configPath);
    }

    void refreshOne(Repository& r) {
        r.resolveError.clear();
        if (!isHttpsUrl(r.url)) {
            r.resolveError = "unsupported URL scheme (https required in v1)";
            return;
        }
        std::string body;
        const FetchResult result = fetcher->get(r.url, body);
        if (!result.ok) {
            r.resolveError = "fetch failed: " + r.url;
            if (!result.error.empty())  {
                r.resolveError += " — " + result.error;
            }

            return;
        }
        std::string err;
        Repository parsed = r;  // keep url/enabled/isDefault
        parsed.name.clear();
        parsed.displayName.clear();
        parsed.description.clear();
        parsed.homepage.clear();
        parsed.indexUrl.clear();
        parsed.trustedSignerDids.clear();
        if (!parseLogosRepoJson(body, parsed, err)) {
            r.resolveError = "logos-repo.json: " + err;
            return;
        }
        r = std::move(parsed);
    }
};

RepositoryRegistry::RepositoryRegistry() : impl_(std::make_unique<Impl>()) {}

RepositoryRegistry::RepositoryRegistry(std::string configPath)
    : impl_(std::make_unique<Impl>()) {
    impl_->configPath = std::move(configPath);
    impl_->persistent = !impl_->configPath.empty();
    impl_->load();
}

RepositoryRegistry::~RepositoryRegistry() = default;

void RepositoryRegistry::setFetcher(std::shared_ptr<Fetcher> fetcher) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->fetcher = std::move(fetcher);
}

std::vector<Repository> RepositoryRegistry::list() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    std::vector<Repository> out;
    if (!impl_->defaultRemoved) {
        Repository defCopy = impl_->defaultRepo;
        defCopy.enabled = !impl_->defaultDisabled;
        out.push_back(std::move(defCopy));
    }
    for (const auto& r : impl_->userRepos) out.push_back(r);
    return out;
}

std::string RepositoryRegistry::addRepository(const std::string& url) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (!impl_->persistent) return "no config file (pass --config <path>)";
    if (url.empty()) return "url is empty";
    if (url == impl_->defaultRepo.url) {
        if (!impl_->defaultRemoved && !impl_->defaultDisabled)
            return "already registered: " + url;
        Repository probe = impl_->defaultRepo;
        probe.enabled = true;
        impl_->refreshOne(probe);
        if (!probe.resolveError.empty()) return probe.resolveError;
        impl_->defaultRepo     = std::move(probe);
        impl_->defaultRemoved  = false;
        impl_->defaultDisabled = false;
        return impl_->save();
    }
    if (!isHttpsUrl(url)) return "unsupported URL scheme (https required in v1)";
    for (const auto& r : impl_->userRepos) {
        if (r.url == url) return "already registered: " + url;
    }
    Repository r;
    r.url = url;
    r.enabled = true;
    // Try to resolve metadata now so the caller learns about a bad URL
    // immediately instead of at first list time.
    impl_->refreshOne(r);
    if (!r.resolveError.empty()) return r.resolveError;
    impl_->userRepos.push_back(std::move(r));
    return impl_->save();
}

std::string RepositoryRegistry::removeRepository(const std::string& url) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (!impl_->persistent) return "no config file (pass --config <path>)";
    if (url == impl_->defaultRepo.url) {
        if (impl_->defaultRemoved) return "not registered: " + url;
        impl_->defaultRemoved = true;
        return impl_->save();
    }
    auto it = std::find_if(impl_->userRepos.begin(), impl_->userRepos.end(),
                           [&](const Repository& r) { return r.url == url; });
    if (it == impl_->userRepos.end()) return "not registered: " + url;
    impl_->userRepos.erase(it);
    return impl_->save();
}

std::string RepositoryRegistry::setEnabled(const std::string& url, bool enabled) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (!impl_->persistent) return "no config file (pass --config <path>)";
    if (url == impl_->defaultRepo.url) {
        if (impl_->defaultRemoved) return "not registered: " + url;
        impl_->defaultDisabled = !enabled;
        return impl_->save();
    }
    for (auto& r : impl_->userRepos) {
        if (r.url == url) {
            r.enabled = enabled;
            return impl_->save();
        }
    }
    return "not registered: " + url;
}

std::string RepositoryRegistry::refresh() {
    std::lock_guard<std::mutex> lock(impl_->mu);
    std::vector<std::string> errs;
    if (!impl_->defaultRemoved) {
        impl_->refreshOne(impl_->defaultRepo);
        if (!impl_->defaultRepo.resolveError.empty()) {
            errs.push_back("default: " + impl_->defaultRepo.resolveError);
        }
    }
    for (auto& r : impl_->userRepos) {
        impl_->refreshOne(r);
        if (!r.resolveError.empty()) errs.push_back(r.url + ": " + r.resolveError);
    }
    if (errs.empty()) return {};
    std::string out;
    for (auto& e : errs) { out += e; out += '\n'; }
    return out;
}

std::optional<Repository> RepositoryRegistry::findByUrlOrName(const std::string& s) const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto match = [&](const Repository& r) {
        return r.url == s || (!r.name.empty() && r.name == s);
    };
    if (match(impl_->defaultRepo)) {
        if (impl_->defaultRemoved) return std::nullopt;
        Repository copy = impl_->defaultRepo;
        copy.enabled = !impl_->defaultDisabled;
        return copy;
    }
    for (const auto& r : impl_->userRepos) {
        if (match(r)) return r;
    }
    return std::nullopt;
}

bool RepositoryRegistry::isPersistent() const { return impl_->persistent; }
std::string RepositoryRegistry::configPath() const { return impl_->configPath; }

// ─── PackageDownloaderLib::Impl ───────────────────────────────────────────────

struct PackageDownloaderLib::Impl {
    RepositoryRegistry registry;
    std::shared_ptr<Fetcher> fetcher = std::make_shared<HttpsFetcher>();

    // Caches: url -> body
    std::unordered_map<std::string, std::string> indexJsonByRepoUrl;
    // True once registry.refresh() has resolved every repo's
    // logos-repo.json this process. Guards ensureMetadata so a catalog
    // browse doesn't re-fetch all repo metadata on every call. Reset by
    // setFetcher (new fetcher → must re-resolve) and forced by
    // refreshCatalogs (explicit reload).
    bool metadataResolved = false;

    mutable std::mutex mu;

    Impl() {}
    explicit Impl(std::string configPath) : registry(std::move(configPath)) {}

    bool ensureMetadata() {
        std::lock_guard<std::mutex> lock(mu);

        // Lazy: resolve repo metadata once per process. The first
        // catalog/list/resolve call pays the network cost; later calls
        // reuse it. Best-effort — refresh() records per-repo errors and
        // never throws, so a partial failure still leaves the working
        // repos usable. Use refreshCatalogs() to force a re-fetch.
        if (metadataResolved) return true;
        registry.refresh();
        metadataResolved = true;
        return true;
    }

    // `err` receives the transport's reason when the index cannot be read.
    // Without it an unreachable catalog is indistinguishable from one that
    // simply does not carry the package.
    std::string fetchIndex(const Repository& r, std::string* err = nullptr) {
        if (r.indexUrl.empty()) {
            if (err) *err = "repository metadata declares no indexUrl";
            return {};
        }

        {
            std::lock_guard<std::mutex> lock(mu);
            auto it = indexJsonByRepoUrl.find(r.url);
            if (it != indexJsonByRepoUrl.end()) return it->second;
        }
        std::string body;
        FetchResult result = fetcher->get(r.indexUrl, body);

        if (!result.ok) {
            if (err)
                *err = "index fetch failed: " + r.indexUrl +
                       (result.error.empty() ? "" : " - " + result.error);
            return {};
        }

        std::lock_guard<std::mutex> lock(mu);
        indexJsonByRepoUrl[r.url] = body;
        return body;
    }

    void clearCaches() {
        indexJsonByRepoUrl.clear();
    }

    // Synthesise the catalog entries for one repo from its index.json
    // body and append them to `out`. Single source of truth for the
    // entry shape so getCatalogJson (all repos) and getCatalogForRepoJson
    // (one repo) can't diverge — the latter previously returned the raw
    // index `packages[]`, missing the synthesised repositoryUrl /
    // repository{Name,DisplayName} / package-level type/category/etc.
    // and the date-sorted versions that callers (CLI/UI/C API) expect.
    void appendCatalogEntries(const Repository& r, const std::string& body, json& out) {
        if (body.empty()) return;
        try {
            auto idx = json::parse(body);
            if (!idx.is_object() || !idx.contains("packages")
                || !idx["packages"].is_array()) return;
            for (auto& pkg : idx["packages"]) {
                if (!pkg.is_object() || !pkg.contains("name")) continue;
                json entry;
                entry["repositoryUrl"]  = r.url;
                entry["repositoryName"] = r.name.empty() ? r.url : r.name;
                entry["repositoryDisplayName"] = r.displayName;
                entry["name"] = pkg["name"];
                auto versions = pkg.value("versions", json::array());
                std::stable_sort(versions.begin(), versions.end(), VersionPrecedenceDesc{});
                // Package "header" fields lifted from the NEWEST version's
                // embedded manifest. Most are constant across a package's
                // versions; `variants` is the one that genuinely moves — a
                // package gains a target in a release — and the newest is the
                // one an install picks up, so it is the one a row answers
                // "will this run on my device" with. The older lists stay
                // visible under versions[].manifest.main.
                if (!versions.empty()) {
                    const json& firstVersion = versions[0];
                    const json& firstManifest = objOrEmpty(firstVersion, "manifest");
                    entry["displayName"] = firstManifest.value("display_name", "");
                    entry["description"] = firstManifest.value("description", "");
                    entry["type"]        = firstManifest.value("type", "");
                    entry["category"]    = firstManifest.value("category", "");
                    entry["author"]      = firstManifest.value("author", "");
                    entry["manifestVersion"] = firstManifest.value("manifestVersion", "");
                    entry["provides"] = firstManifest.value("provides", "");
                    const std::string iconPath =
                        objOrEmpty(firstVersion, "icon").value("path", "");
                    const auto slash = r.indexUrl.find_last_of('/');
                    if (!iconPath.empty() && slash != std::string::npos) {
                        entry["icon"] = r.indexUrl.substr(0, slash) + "/" + iconPath;
                    }
                }
                // Always present, even when empty: a consumer filtering rows by
                // platform needs a list to read, not a key to test for. `main`
                // also has a plain-string form and an index row may carry no
                // manifest at all — neither is a variant list.
                entry["variants"] = versions.empty()
                                  ? json::array()
                                  : variantsOf(objOrEmpty(versions[0], "manifest"));
                entry["versions"] = std::move(versions);
                out.push_back(std::move(entry));
            }
        } catch (...) {
            // Unparseable index; surfaced via resolveError in listRepositoriesJson.
        }
    }
};

PackageDownloaderLib::PackageDownloaderLib()
    : impl_(std::make_unique<Impl>()) {}

PackageDownloaderLib::PackageDownloaderLib(std::string configPath)
    : impl_(std::make_unique<Impl>(std::move(configPath))) {}

PackageDownloaderLib::~PackageDownloaderLib() = default;

void PackageDownloaderLib::setFetcher(std::shared_ptr<Fetcher> fetcher) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->fetcher = fetcher;
    impl_->registry.setFetcher(fetcher);
    impl_->clearCaches();
    // New fetcher → the once-per-process metadata resolution must run
    // again (against the new fetcher) on the next ensureMetadata.
    impl_->metadataResolved = false;
}

RepositoryRegistry& PackageDownloaderLib::registry() { return impl_->registry; }
const RepositoryRegistry& PackageDownloaderLib::registry() const { return impl_->registry; }

std::string PackageDownloaderLib::listRepositoriesJson() {
    impl_->ensureMetadata();
    json arr = json::array();
    for (const auto& r : impl_->registry.list()) {
        json e;
        e["url"] = r.url;
        e["enabled"] = r.enabled;
        e["isDefault"] = r.isDefault;
        e["name"] = r.name;
        e["displayName"] = r.displayName;
        e["description"] = r.description;
        e["homepage"] = r.homepage;
        e["indexUrl"] = r.indexUrl;
        e["trustedSignerDids"] = r.trustedSignerDids;
        e["resolveError"] = r.resolveError;
        arr.push_back(std::move(e));
    }
    return arr.dump();
}

std::string PackageDownloaderLib::getCatalogJson() {
    impl_->ensureMetadata();
    json out = json::array();
    for (const auto& r : impl_->registry.list()) {
        if (!r.enabled) continue;
        if (!r.resolveError.empty()) continue;
        impl_->appendCatalogEntries(r, impl_->fetchIndex(r), out);
    }
    return out.dump();
}

std::string PackageDownloaderLib::getCatalogForRepoJson(const std::string& urlOrName) {
    auto repo = impl_->registry.findByUrlOrName(urlOrName);
    if (!repo) return "[]";
    impl_->ensureMetadata();
    // Same synthesised shape as getCatalogJson, scoped to one repo —
    // callers (CLI `--repo`, the UI, the C API) get repositoryUrl,
    // date-sorted versions, and the package-level header fields, not the
    // raw index `packages[]`.
    json out = json::array();
    impl_->appendCatalogEntries(*repo, impl_->fetchIndex(*repo), out);
    return out.dump();
}

std::string PackageDownloaderLib::refreshCatalogs() {
    // Explicit reload: force the metadata refresh now and mark it done
    // so the next ensureMetadata() doesn't redundantly refresh again.
    std::string out = impl_->registry.refresh();

    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->clearCaches();
        impl_->metadataResolved = true;
    }

    // Then re-fetch the indexes themselves. refresh() above resolves only
    // logos-repo.json, so a catalog whose index.json was unreachable still
    // reported success here while every later browse quietly returned
    // nothing. Not holding mu: fetchIndex takes it.
    for (const auto& r : impl_->registry.list()) {
        if (!r.enabled || !r.resolveError.empty()) continue;
        std::string err;
        if (impl_->fetchIndex(r, &err).empty())
            out += r.url + ": " + (err.empty() ? "index unavailable" : err) + "\n";
    }
    return out;
}

namespace {

// Find a version entry within a package by version+rootHash. Returns nullptr
// if no candidate matches.
const json* pickVersion(const json& pkg,
                        const std::string& version,
                        const std::string& rootHash) {
    if (!pkg.is_object() || !pkg.contains("versions") || !pkg["versions"].is_array())
        return nullptr;
    const json* best = nullptr;
    std::string bestVer;
    std::string bestDate;
    for (const auto& v : pkg["versions"]) {
        // Skip null/array/scalar entries — value() on a non-object throws
        // type_error 306. The catalog may have been generated by an older
        // index-builder that wrote partial entries; better to skip than crash.
        if (!v.is_object()) continue;
        std::string vVer  = objOrEmpty(v, "manifest").value("version", "");
        std::string vHash = v.value("rootHash", "");
        std::string vDate = v.value("releasedAt", "");
        if (!version.empty() && vVer != version) continue;
        if (!rootHash.empty() && vHash != rootHash) continue;
        // "Newest" = highest SemVer precedence, with releasedAt only breaking
        // ties (see PackageDownloaderLib::outranks). This path picks the version
        // when the caller pins none (version=""), so it must use the same rule
        // as findBest and the catalog sort — otherwise downloadPackage(...,"")
        // would still prefer a later-published LOWER version.
        if (!best || PackageDownloaderLib::outranks(vVer, vDate, bestVer, bestDate)) {
            best = &v;
            bestVer = vVer;
            bestDate = vDate;
        }
    }
    return best;
}

} // namespace

namespace {

// Verify that a freshly-downloaded `.lgx` is structurally sound AND is
// the artifact the catalog advertised.
//
// Two layers:
//  1. Structural — lgx_verify confirms the file is a well-formed .lgx
//     whose internal content hashes are self-consistent. Catches a
//     truncated / corrupt download (or a non-.lgx file served at the
//     URL) before it ever reaches the installer. Runs unconditionally.
//  2. Index→file binding — the index.json comes from one host, the
//     .lgx `url` from another; nothing otherwise stops a swapped file
//     (a downgrade attack, or a different-but-also-signed sibling
//     package). We bind them three ways:
//       * rootHash  — the catalog pins a version's `rootHash`; the
//                     .lgx's own manifest carries the same value under
//                     `hashes.root` (lgx recomputes the Merkle tree on
//                     every content change and records it in the
//                     manifest). A plain string compare of the two
//                     binds the download to the advertised content —
//                     no recomputation here.
//       * manifest  — name / version / main / dependencies / type must
//                     match the manifest the catalog embedded.
//       * signer    — if the catalog advertised a signer DID, the file
//                     must be signed by the SAME DID.
//
// Ed25519 trust (is the signer in our keyring) stays package_manager's
// job at install time — here we only confirm the file is what the
// index said it would be.
//
// `indexEntry` is the catalog version entry — `{ manifest, signature,
// rootHash, url, ... }`.
//
// Returns true (accept) on a match, or when the index carried nothing
// to compare a given facet against (legacy rows with `manifest: null`
// / no `rootHash` — we can't verify what wasn't advertised). Returns
// false (reject) on a real mismatch, with `errMsg` set.
bool verifyDownloadAgainstIndex(const std::string& lgxPath,
                                const json& indexEntry,
                                std::string& errMsg) {
    // ── 1. Structural soundness ──────────────────────────────────────
    {
        lgx_verify_result_t vr = lgx_verify(lgxPath.c_str());
        const bool ok = vr.valid;
        std::string firstErr;
        if (!ok && vr.errors && vr.errors[0])
            firstErr = vr.errors[0];
        lgx_free_verify_result(vr);
        if (!ok) {
            errMsg = "downloaded file failed .lgx structure verification"
                   + (firstErr.empty() ? std::string()
                                       : (": " + firstErr));
            return false;
        }
    }

    // Load the package + read its manifest once — both binding checks
    // below come straight out of it.
    lgx_package_t pkg = lgx_load(lgxPath.c_str());
    if (!pkg) {
        errMsg = "downloaded file is not a readable .lgx package";
        return false;
    }
    const char* mraw = lgx_get_manifest_json(pkg);
    std::string fileManifestStr = mraw ? mraw : "";
    lgx_free_package(pkg);

    json fileManifest;
    try { fileManifest = json::parse(fileManifestStr); }
    catch (...) {
        errMsg = "downloaded package has an unparseable manifest";
        return false;
    }

    // ── 2a. rootHash binding ─────────────────────────────────────────
    // The catalog pins a version's `rootHash`; the .lgx's manifest
    // records the same Merkle tree under `hashes`, with `hashes.root`
    // the content root. A plain string compare binds the download to
    // the advertised content — no recomputation. (lgx_verify above
    // already confirmed the manifest's recorded hashes are consistent
    // with the actual archive content, so a manifest-stated root we
    // can trust as the file's real root.)
    const std::string advRootHash = indexEntry.value("rootHash", "");
    if (!advRootHash.empty()) {
        const std::string fileRootHash =
            objOrEmpty(fileManifest, "hashes").value("root", "");
        if (fileRootHash != advRootHash) {
            errMsg = "downloaded package content hash does not match the "
                     "catalog (expected " + advRootHash + ", got "
                   + (fileRootHash.empty() ? "none" : fileRootHash) + ")";
            return false;
        }
    }

    // ── 2b. Manifest binding ─────────────────────────────────────────
    // Skipped when the index row has no manifest (the `manifest: null`
    // rows objOrEmpty was written for).
    const json& advertised = objOrEmpty(indexEntry, "manifest");
    if (!advertised.empty()) {
        // Compare the security-relevant fields rather than requiring
        // whole-manifest byte-equality: the index builder may
        // normalise the manifest copy it embeds (key order, an added
        // display-only field), and a benign cosmetic difference
        // shouldn't block an install. These are the fields an
        // index→file swap would have to alter to be an actual attack:
        //   name / version — identity
        //   main           — variant→entrypoint map (the code that loads)
        //   dependencies   — the transitive surface
        // `type` is included too — cheap, and a ui_qml/core swap is
        // load-path-relevant.
        static const char* kFields[] = {
            "name", "version", "main", "dependencies", "type"};
        for (const char* f : kFields) {
            const json a = advertised.contains(f)   ? advertised.at(f)   : json();
            const json b = fileManifest.contains(f) ? fileManifest.at(f) : json();
            if (a != b) {
                errMsg = std::string("downloaded package field '") + f
                       + "' does not match the catalog entry";
                return false;
            }
        }
    }

    // ── 2c. Signer binding ───────────────────────────────────────────
    // When the index advertised a signer DID, the file must be signed
    // by the SAME DID — and the signature must actually VERIFY.
    //
    // `signature_valid`, not `is_signed`, and never `signer_did` on its
    // own. logos-package populates signer_did straight out of
    // manifest.sig BEFORE it runs the Ed25519 check
    // (Package::verifySignature sets info.signer_did, then may return
    // early on a bad DID, a malformed signature, or a failed verify),
    // so signer_did is a CLAIM the package makes about itself until
    // that check passes. This block used to read only is_signed and
    // signer_did, so a substituted package carrying a hand-written
    // manifest.sig that merely NAMED the advertised DID satisfied the
    // binding: an attacker did not need the key, only the DID string,
    // which the catalog publishes.
    //
    // Verifying here does NOT make this an authorization check — no
    // keyring is consulted and none should be; the trust-anchor gate in
    // logos-package-manager owns that. All this asks is whether the
    // bytes we downloaded really are the ones the advertised publisher
    // signed, which is the only reading under which "binds index→file"
    // is true at all.
    const json& advSig = objOrEmpty(indexEntry, "signature");
    if (!advSig.empty()) {
        const std::string advDid = advSig.value("did", "");
        if (!advDid.empty()) {
            lgx_signature_info_t info =
                lgx_verify_signature(lgxPath.c_str(), nullptr);
            const bool fileSigned = info.is_signed;
            const bool fileSigValid = info.signature_valid;
            const std::string fileDid =
                info.signer_did ? info.signer_did : "";
            lgx_free_signature_info(info);
            if (!PackageDownloaderLib::downloadedSignerBinds(
                    fileSigned, fileSigValid, fileDid, advDid)) {
                errMsg = "downloaded package signer does not match the "
                         "catalog (expected " + advDid;
                // Name WHICH of the three it was: "expected X" alone
                // reads as a DID mismatch, and sends whoever hits the
                // forged case looking for the wrong problem entirely.
                if (!fileSigned)
                    errMsg += ", file is unsigned)";
                else if (!fileSigValid)
                    errMsg += ", file claims " + (fileDid.empty() ? std::string("no DID") : fileDid)
                            + " but its signature does not verify)";
                else
                    errMsg += ", got " + fileDid + ")";
                return false;
            }
        }
    }

    return true;
}

// Private staging directory used when the caller names no outputDir.
//
// The published filename is derived from the package and version, so in the
// shared temp root two accounts on one host race for a single path. The
// loser cannot overwrite a 0644 file it does not own and, because the temp
// root is sticky, cannot rename over or unlink it either -- wedging that
// package for that user permanently. A per-uid directory removes the shared
// name entirely; it is created 0700 and rejected unless we own it, so it
// cannot be squatted by another account either.
std::string stagingDir(std::string& err) {
    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path(ec);
    if (ec || tmp.empty()) {
        err = "no usable temporary directory" +
              (ec ? ": " + ec.message() : std::string());
        return {};
    }
#ifdef _WIN32
    // Each account already gets its own %TEMP%, so there is nothing to key on.
    const fs::path dir = tmp / "lgpd";
    fs::create_directories(dir, ec);
    if (ec) {
        err = "cannot create " + dir.string() + ": " + ec.message();
        return {};
    }
#else
    const fs::path dir =
        tmp / ("lgpd-" + std::to_string(static_cast<unsigned long>(::geteuid())));
    if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        err = "cannot create " + dir.string() + ": " + std::strerror(errno);
        return {};
    }
    // lstat, not stat: a symlink planted at this path must not be followed.
    struct stat st {};
    if (::lstat(dir.c_str(), &st) != 0) {
        err = "cannot stat " + dir.string() + ": " + std::strerror(errno);
        return {};
    }
    if (!S_ISDIR(st.st_mode) || st.st_uid != ::geteuid() ||
        (st.st_mode & (S_IWGRP | S_IWOTH))) {
        err = dir.string() + " is not a private directory owned by this user; "
              "remove it or set TMPDIR";
        return {};
    }
#endif
    return dir.string();
}

// Versions a package actually offers, for the "you asked for X" message.
std::string offeredVersions(const json& pkg) {
    std::vector<std::string> vs;
    if (pkg.contains("versions") && pkg["versions"].is_array()) {
        for (const auto& v : pkg["versions"]) {
            if (!v.is_object()) continue;
            const std::string s = objOrEmpty(v, "manifest").value("version", "");
            if (!s.empty() && std::find(vs.begin(), vs.end(), s) == vs.end())
                vs.push_back(s);
        }
    }
    if (vs.empty()) return "none";
    std::string out;
    for (size_t i = 0; i < vs.size() && i < 12; ++i) {
        if (i) out += ", ";
        out += vs[i];
    }
    if (vs.size() > 12) out += ", ...";
    return out;
}

} // namespace

std::string PackageDownloaderLib::downloadPackage(const std::string& repoUrlOrName,
                                                  const std::string& packageName,
                                                  std::string& errorMessage,
                                                  const std::string& version,
                                                  const std::string& rootHash,
                                                  const std::string& outputDir,
                                                  const ProgressFn& onProgress) {

    // Clear any previous error message before starting a new download attempt.
    errorMessage.clear();

    impl_->ensureMetadata();

    // Build the list of repos to consider.
    std::vector<Repository> candidates;
    if (repoUrlOrName.empty()) {
        for (const auto& r : impl_->registry.list()) {
            if (r.enabled && r.resolveError.empty()) candidates.push_back(r);
        }
        if (candidates.empty()) {
            errorMessage = "no enabled repository resolved successfully; "
                           "see `catalog ls` for per-repository errors";
            return {};
        }
    } else {
        auto r = impl_->registry.findByUrlOrName(repoUrlOrName);

        if (!r) {
            errorMessage = "no such repository: " + repoUrlOrName;
            return {};
        }

        candidates.push_back(*r);
    }

    // Why each candidate could not serve the package. Collected rather than
    // overwritten so a readable-but-empty repo cannot hide an unreachable one.
    std::vector<std::string> notes;
    bool sawPackage = false;
    bool readAnyIndex = false;

    for (const auto& repo : candidates) {
        std::string fetchErr;
        std::string body = impl_->fetchIndex(repo, &fetchErr);
        if (body.empty()) {
            notes.push_back(repo.url + ": " +
                            (fetchErr.empty() ? "index unavailable" : fetchErr));
            continue;
        }
        json idx;
        try { idx = json::parse(body); }
        catch (const json::exception& e) {
            notes.push_back(repo.url + ": index is not valid JSON: " + e.what());
            continue;
        }
        if (!idx.is_object() || !idx.contains("packages") || !idx["packages"].is_array()) {
            notes.push_back(repo.url + ": index has no 'packages' array");
            continue;
        }
        readAnyIndex = true;
        for (const auto& pkg : idx["packages"]) {
            if (!pkg.is_object() || pkg.value("name", "") != packageName) continue;
            sawPackage = true;
            const json* v = pickVersion(pkg, version, rootHash);
            if (!v || !v->is_object()) {
                notes.push_back(repo.url + ": no release of " + packageName +
                                " matches" +
                                (version.empty() ? "" : " version " + version) +
                                (rootHash.empty() ? "" : " rootHash " + rootHash) +
                                " (offers: " + offeredVersions(pkg) + ")");
                continue;
            }
            std::string url = v->value("url", "");
            if (url.empty()) {
                notes.push_back(repo.url + ": " + packageName +
                                " has no download url for the selected release");
                continue;
            }
            // Derive destination path.
            std::string destDir = outputDir;
            if (destDir.empty()) {
                // stagingDir() honours TMPDIR/TMP/TEMP via
                // temp_directory_path(), then isolates us inside it.
                std::string dirErr;
                destDir = stagingDir(dirErr);
                if (destDir.empty()) {
                    errorMessage = dirErr;
                    return {};
                }
            }
            std::string filename = fs::path(url).filename().string();
            if (filename.empty()) filename = packageName + ".lgx";
            std::string dest = (fs::path(destDir) / filename).string();

            // Use a random suffix to avoid collisions.
            // This is rare: the filename contains the version.
            // Download the same version twice in parallel is not common.
            std::random_device rd;
            std::ostringstream suffix;

            // Example: 3f9a2c1b.
            suffix << std::hex << rd();

            const std::string pendingFile =
                (fs::path(destDir) / (filename + "." + suffix.str())).string();

            // Known before any byte moves, and still right for a chunked
            // response where curl reports dltotal == 0 throughout. It is also
            // the ONLY denominator a transport with no Content-Length can
            // offer, so it is resolved once here rather than per transport.
            const std::uint64_t advertisedSize = v->value("size", std::uint64_t{0});
            // Rate-limit at the source, not in each caller.
            ProgressThrottle throttle;

            // Built once and shared by every transport that may fetch this
            // package. PASS THIS TO EACH ONE: the two-argument getToFile
            // reports nothing (see Fetcher's progress overload), so a
            // transport wired up with it goes silently progress-less — the
            // bar simply never moves, with no error to notice.
            const ProgressFn progressSink =
                onProgress ? ProgressFn([&](std::uint64_t received,
                                            std::uint64_t total) {
                                 const std::uint64_t denom =
                                     total ? total : advertisedSize;
                                 if (!throttle.shouldEmit(received, denom, monotonicNowMs()))
                                     return;
                                 // Guarded here too, so the "a bad sink never
                                 // fails a good download" guarantee holds for
                                 // any Fetcher, not just the libcurl one whose
                                 // trampoline also catches.
                                 try {
                                     onProgress(received, denom);
                                 } catch (...) {
                                 }
                             })
                           : ProgressFn{};

            const FetchResult fetched =
                impl_->fetcher->getToFile(url, pendingFile, progressSink);

            if (!fetched.ok) {
                std::error_code rmEc;
                fs::remove(pendingFile, rmEc);
                errorMessage = "download of " + packageName + " from " + url
                             + " failed: " + fetched.error;
                return {};
            }
            // Bind the downloaded artifact to what the index advertised
            // — manifest fields + signer DID. The .lgx `url` and the
            // `index.json` come from independent hosts; without this a
            // swapped file (downgrade attack, sibling package) would
            // sail through to install. Deep integrity + Ed25519 trust
            // stay package_manager's job at install time; this is only
            // the index→file binding. On mismatch we delete the bad
            // artifact and fail the download.
            {
                std::string verr;
                if (!verifyDownloadAgainstIndex(pendingFile, *v, verr)) {
                    std::error_code rmEc;
                    fs::remove(pendingFile, rmEc);
                    errorMessage = "rejected " + packageName + " from " + url
                                 + ": " + verr;
                    return {};
                }
            }

            std::error_code mvEc;
            fs::rename(pendingFile, dest, mvEc);

            if (mvEc) {
                std::error_code rmEc;
                fs::remove(pendingFile, rmEc);
                errorMessage = "cannot publish " + packageName + " to " + dest
                             + ": " + mvEc.message();
                return {};
            }
            return dest;
        }
    }
    if (errorMessage.empty()) {
        if (sawPackage)
            errorMessage = "no release of " + packageName +
                           " matched the requested pin";
        else if (!readAnyIndex)
            // Saying "nobody advertises it" when we never read an index sends
            // the reader after the wrong problem.
            errorMessage = "could not read any repository index while looking "
                           "for " + packageName;
        else
            errorMessage = "no repository advertises " + packageName +
                           (version.empty() ? "" : " " + version);
        for (const auto& n : notes) errorMessage += "\n  " + n;
    }
    return {};
}

namespace {

// Parse one manifest dependency entry — accepts a JSON string or object.
struct ParsedDep {
    std::string name;
    std::optional<std::string> versionRange;
    std::optional<std::string> signer;
    // Optional source-repo scope. When set, findBest only considers
    // packages from this repository URL — so callers that already know
    // exactly which repo entry they want (the package_manager_ui's
    // per-row install / upgrade path, scoped to the row the user
    // clicked) can pin it. Manifest-declared transitive deps don't set
    // this; they fall through to the cross-repo "best" pick.
    std::optional<std::string> repositoryUrl;
};

// A dependency's `signer` field DISAMBIGUATES among same-named candidates: "of
// the several packages called `bm` in the merged catalog, I mean the one this
// identity published". IT IS NOT AN AUTHORIZATION. Matching a pin does not make
// a package installable; that is the install-time TRUST-ANCHOR POLICY in
// logos-package-manager, which refuses a package no ACTIVE anchor validates.
// Both checks are needed and they are not the same check — a bare identifier in
// a manifest or a catalog entry establishes no trust anchor, so nothing this
// function accepts may ever be read as permission to install.
//
// A did:jwk that is syntactically well-formed, matched against a catalog row,
// and freely chosen by whoever wrote the manifest. That is all it is.
bool isValidDidJwk(const std::string& s) {
    // Same predicate as logos-package's Manifest::validate (src/core/manifest.cpp:29),
    // which is what `lgx verify` enforces on a package's own manifest. It is
    // duplicated rather than shared because logos-package does not export it —
    // one spec, two implementations; keep them in step.
    static const std::regex re("^did:jwk:[A-Za-z0-9_-]+$");
    return std::regex_match(s, re);
}

bool parseDep(const json& j, ParsedDep& out, std::string& err) {
    if (j.is_string()) { out.name = j.get<std::string>(); return true; }
    if (!j.is_object()) { err = "dependency entry must be string or object"; return false; }
    if (!j.contains("name") || !j["name"].is_string()) {
        err = "dependency object missing 'name'"; return false;
    }
    out.name = j["name"].get<std::string>();
    if (j.contains("version") && j["version"].is_string())
        out.versionRange = j["version"].get<std::string>();

    // `signer` — REGRESSION (B1). This used to read
    //
    //     if (j.contains("signer") && j["signer"].is_string())
    //         out.signer = j["signer"].get<std::string>();
    //
    // so `"signer": ""` parsed clean with the option ENGAGED holding "".
    // findBest then compared that against each candidate's `signature.did`,
    // and an UNSIGNED row yields "" — so the pin matched exactly the rows with
    // NO signature and skipped every signed one. Against a catalog holding
    // bm 2.0.0 (signed) and bm 1.0.0 (unsigned), `{"name":"bm","signer":""}`
    // resolved to the unsigned, OLDER 1.0.0, with no error. A signer pin must
    // select AMONG candidates; it must never be a route TO the unsigned ones.
    //
    // And a non-string or null `signer` fell through the `is_string()` guard
    // and left the dep UNPINNED — a declared constraint silently widened to
    // "anything", which is the one outcome a pin exists to prevent.
    //
    // THE RULE, for this field: absent means unpinned; PRESENT means it must be
    // a syntactically valid did:jwk. Null, "", a non-string and a malformed DID
    // are all hard errors. There is no shape of this key that quietly widens
    // the candidate set, and none that narrows it onto the unsigned rows.
    //
    // Nothing upstream would catch it either: `lgx verify` runs
    // Manifest::validate on a package's own manifest, but this resolver reads
    // catalog-EMBEDDED manifests and a caller-supplied top-level array, and
    // validates neither. This is the only gate on both paths.
    if (j.contains("signer")) {
        if (!j["signer"].is_string()) {
            err = "dependency '" + out.name + "' has a non-string 'signer' — omit the "
                  "field to leave the signer unpinned";
            return false;
        }
        const std::string signer = j["signer"].get<std::string>();
        if (signer.empty()) {
            err = "dependency '" + out.name + "' declares an empty 'signer' — omit the "
                  "field to leave the signer unpinned; an empty pin is not 'no pin', "
                  "it selects the releases that carry no signature";
            return false;
        }
        if (!isValidDidJwk(signer)) {
            err = "dependency '" + out.name + "' declares a malformed 'signer' DID '"
                + signer + "' — expected did:jwk:<base64url>";
            return false;
        }
        out.signer = signer;
    }

    if (j.contains("repositoryUrl") && j["repositoryUrl"].is_string())
        out.repositoryUrl = j["repositoryUrl"].get<std::string>();
    return true;
}

} // namespace

std::string PackageDownloaderLib::resolveDependenciesJson(const std::string& dependenciesJson,
                                                          const std::string& installedPackagesJson) {
    impl_->ensureMetadata();
    json input;
    try { input = json::parse(dependenciesJson); }
    catch (...) { return R"([{"error":"invalid JSON input"}])"; }
    if (!input.is_array())
        return R"([{"error":"input must be a JSON array"}])";

    // Fetch full merged catalog once.
    std::string catBody = getCatalogJson();
    json cat;
    try { cat = json::parse(catBody); } catch (...) { cat = json::array(); }

    // Installed-packages index, name → version. Used to short-circuit
    // transitive deps whose range is already satisfied by what's on
    // disk. Empty / unparseable installedPackagesJson disables the
    // optimisation — the resolver then picks every transitive from the
    // catalog (pre-installed-aware behaviour).
    std::unordered_map<std::string, std::string> installedByName;
    if (!installedPackagesJson.empty()) {
        try {
            json inst = json::parse(installedPackagesJson);
            if (inst.is_array()) {
                for (const auto& e : inst) {
                    if (!e.is_object()) continue;
                    const std::string n = e.value("name", "");
                    const std::string v = e.value("version", "");
                    if (!n.empty() && !v.empty()) installedByName[n] = v;
                }
            }
        } catch (...) { /* silent — best effort */ }
    }

    json out = json::array();
    std::unordered_map<std::string, bool> seen; // name|version|hash
    // Names resolved from an explicit top-level input, and the version chosen
    // for each. A top-level pin is authoritative: the same module must not then
    // be re-resolved to a *different* (newest) version when it also turns up as
    // a transitive dependency of another top-level input. See the transitive
    // guard below.
    std::unordered_map<std::string, std::string> topLevelChosen; // name -> version

    // Lookup helper: find best candidate across repos.
    auto findBest = [&](const ParsedDep& dep, json& chosen, std::string& chosenRepo, std::string& errMsg) -> bool {
        // Hold the best candidate as an owned value, NOT a pointer.
        //
        // The previous `const json* bestVer = &v` aliased an element of
        // the per-package `versions` sequence. With the ternary below,
        // `versions` is a *temporary copy* whose lifetime ends with the
        // enclosing `for (pkg : cat)` iteration. When the same package
        // name appears in more than one repository (e.g. the official
        // repo plus a user fork both publishing `wallet_module`), the
        // best candidate is found in repo A, the loop advances to
        // repo B's same-named package, repo A's `versions` copy is
        // destroyed, and `*bestVer` afterwards dereferences freed
        // memory. The corrupted read surfaced downstream as
        // `json type_error.306 (value() with null)` on the next
        // `chosen.value(...)`. Copying the chosen version out on the
        // spot removes the alias entirely.
        bool haveBest = false;
        json bestVer;
        std::string bestRepo;
        std::string bestDate;
        std::string bestVersion;
        // Stable empty array so the ternary binds a real lvalue ref
        // (no materialised temporary) when `versions` is absent/null.
        const json emptyArr = json::array();
        for (const auto& pkg : cat) {
            if (!pkg.is_object() || pkg.value("name", "") != dep.name) continue;
            // Repo scope. When the caller pinned a repositoryUrl (the
            // per-row install path in package_manager_ui does this with
            // the row's source repo), skip packages from other repos
            // — otherwise two repos publishing the same `name` would be
            // ranked against each other and the resolver could pick the
            // wrong one. Empty pin = no scope = pre-fix cross-repo
            // behaviour, matching manifest-declared transitive deps.
            if (dep.repositoryUrl && pkg.value("repositoryUrl", "") != *dep.repositoryUrl)
                continue;
            // `versions` may legally be an array, missing, or
            // (defensively) null — only the array case is iterable.
            const json& versions = (pkg.contains("versions") && pkg["versions"].is_array())
                                   ? pkg["versions"] : emptyArr;
            for (const auto& v : versions) {
                if (!v.is_object()) continue;
                std::string ver = objOrEmpty(v, "manifest").value("version", "");
                if (dep.versionRange && !semverRangeMatches(*dep.versionRange, ver)) continue;
                // Signer pin — SELECTION ONLY. This narrows the candidate set
                // and does nothing else: no keyring is consulted here, no
                // anchor set exists in this process, and a satisfied pin
                // produces an ordinary resolved entry that the installer will
                // still judge on its own terms.
                //
                // parseDep already refuses an empty pin, but that is input
                // validation on one call path. signerPinMatches is the
                // invariant itself, sitting next to the comparison it
                // constrains, so a ParsedDep constructed some other way cannot
                // reopen B1.
                if (dep.signer) {
                    const std::string sigDid = objOrEmpty(v, "signature").value("did", "");
                    if (!PackageDownloaderLib::signerPinMatches(*dep.signer, sigDid)) continue;
                }
                // Rank by SemVer precedence, NOT by release date — see
                // PackageDownloaderLib::outranks.
                std::string date = v.value("releasedAt", "");
                if (!haveBest || PackageDownloaderLib::outranks(ver, date, bestVersion, bestDate)) {
                    haveBest = true;
                    bestVer = v;            // copy — outlives this iteration
                    bestRepo = pkg.value("repositoryUrl", "");
                    bestDate = date;
                    bestVersion = ver;
                }
            }
        }
        if (!haveBest) {
            std::ostringstream oss;
            oss << "no candidate matches '" << dep.name << "'";
            if (dep.versionRange) oss << " @ " << *dep.versionRange;
            if (dep.signer) oss << " (signer=" << *dep.signer << ")";
            if (dep.repositoryUrl) oss << " (repo=" << *dep.repositoryUrl << ")";
            errMsg = oss.str();
            return false;
        }
        chosen = std::move(bestVer);
        chosenRepo = bestRepo;
        return true;
    };

    // BFS so deps-of-deps are added before their consumers. Each queue
    // entry carries an isTopLevel flag — true for the caller's input
    // array, false for deps the resolver pulled in. Top-level entries
    // are NEVER short-circuited by the installed-state check (the user
    // explicitly picked them), and they get `topLevel: true` in the
    // output so consumers can split the resolved list into "the action
    // I asked for" vs "the transitive deps that come along".
    struct QueueEntry { ParsedDep dep; bool isTopLevel; };
    std::vector<QueueEntry> queue;
    for (const auto& el : input) {
        ParsedDep d; std::string err;
        if (!parseDep(el, d, err)) {
            json e; e["error"] = err; out.push_back(std::move(e));
            return out.dump();
        }
        queue.push_back({std::move(d), /*isTopLevel=*/true});
    }

    // Index-based FIFO consumption rather than erase(begin()): the
    // queue only grows (transitive deps are appended), so a head index
    // walks it in O(1) per step. erase(begin()) was O(n) per pop —
    // quadratic on a deep/wide dependency graph. Entries are moved out
    // and never revisited, so the moved-from slots are harmless.
    for (size_t head = 0; head < queue.size(); ++head) {
        QueueEntry qe = std::move(queue[head]);
        const ParsedDep& dep = qe.dep;

        // Installed-state short-circuit (transitive only). If an
        // installed copy's version meets the dep's range, the dep is
        // satisfied as-is; skip emitting an entry and skip recursing
        // into the chosen manifest (we don't have one — by definition
        // we didn't pick from the catalog). Top-level inputs bypass
        // this: the caller asked for them explicitly, so they must
        // always resolve to a catalog pick.
        if (!qe.isTopLevel) {
            // Top-level pin wins. If this same name was explicitly requested at
            // top level, that pick is authoritative — do NOT re-resolve it here
            // to a newer catalog version. Otherwise a module that is both
            // explicitly pinned AND a dependency of another pinned package gets
            // emitted twice (once at the pin, once at newest), and consumers
            // that key rows by name see the newest copy clobber the user's pick
            // (the app-manager version dropdown snapping back to the newest
            // release). The pinned version's own transitive deps were already
            // enqueued when it was processed as a top-level input, so nothing is
            // lost by skipping the duplicate. Whether the pin satisfies this
            // dependent's range is a UI concern (surfaced as an "outdated"
            // hint); we don't block a deliberate pin here.
            if (topLevelChosen.count(dep.name)) continue;

            auto it = installedByName.find(dep.name);
            if (it != installedByName.end()) {
                const bool inRange = !dep.versionRange
                                  || semverRangeMatches(*dep.versionRange, it->second);
                if (inRange) continue;
            }
        }

        json chosen; std::string chosenRepo; std::string err;
        if (!findBest(dep, chosen, chosenRepo, err)) {
            json e; e["error"] = err; e["name"] = dep.name; out.push_back(std::move(e));
            return out.dump();
        }
        const json& chosenManifest = objOrEmpty(chosen, "manifest");
        std::string ver = chosenManifest.value("version", "");
        std::string hash = chosen.value("rootHash", "");
        std::string key = dep.name + "|" + ver + "|" + hash;
        if (seen[key]) continue;
        seen[key] = true;
        json entry;
        entry["name"] = dep.name;
        entry["version"] = ver;
        entry["rootHash"] = hash;
        entry["repositoryUrl"] = chosenRepo;
        entry["url"] = chosen.value("url", "");
        // Lets a caller total an install plan before downloading; 0 when the
        // index omits it.
        entry["size"] = chosen.value("size", std::uint64_t{0});
        entry["topLevel"] = qe.isTopLevel;
        out.push_back(std::move(entry));
        // Remember the version an explicit top-level input resolved to, so a
        // later transitive encounter of the same name defers to it (above).
        if (qe.isTopLevel) topLevelChosen[dep.name] = ver;
        // Enqueue transitive deps from the chosen version's manifest.
        //
        // A dependency entry that cannot be parsed is REPORTED and resolution
        // STOPS. The old `if (parseDep(...)) push` swallowed the failure, so a
        // manifest whose dependency entry had an unusable shape lost the EDGE
        // ITSELF — the dep never entered the queue, never appeared in the plan,
        // and the install proceeded looking complete with a required package
        // missing. (Same defect class as lgpm's manifest scan dropping
        // object-form dependency entries, fixed in logos-package-manager #34.)
        //
        // This matters directly for the signer pin: rejecting `"signer": ""`
        // at parse only helps if the rejection is visible. Dropped, an empty
        // pin in a transitive manifest would go from "silently selects the
        // unsigned release" to "silently drops the dependency" — a different
        // wrong answer, equally quiet.
        //
        // Stopping rather than collecting is deliberate: the return value is
        // an INSTALL PLAN, and a plan with a hole in it must not be executed
        // partially. The top-level loop below already stops on the same
        // condition; matching it keeps one rule for both entry paths.
        if (chosenManifest.contains("dependencies") && chosenManifest["dependencies"].is_array()) {
            for (const auto& sub : chosenManifest["dependencies"]) {
                ParsedDep d; std::string serr;
                if (!parseDep(sub, d, serr)) {
                    json e;
                    e["error"] = "in dependencies of '" + dep.name + "' @ " + ver + ": " + serr;
                    e["name"]  = dep.name;
                    out.push_back(std::move(e));
                    return out.dump();
                }
                queue.push_back({std::move(d), /*isTopLevel=*/false});
            }
        }
    }
    // Reverse so deps appear before their consumers.
    std::reverse(out.begin(), out.end());
    return out.dump();
}

bool PackageDownloaderLib::semverMatches(const std::string& range, const std::string& version) {
    return semverRangeMatches(range, version);
}

bool PackageDownloaderLib::signerPinMatches(const std::string& pin,
                                            const std::string& candidateSignerDid) {
    // Stated positively on purpose. The predicate this replaced was
    // `candidateSignerDid != pin`, which is true-by-accident for the pair
    // ("", "") — an empty pin against a candidate with no signature — and so
    // made an empty pin SELECT exactly the unsigned releases (B1). Written as
    // "both sides must be a real identity, and they must be the same one",
    // that pair cannot arise: neither an empty pin nor an unsigned candidate
    // can satisfy anything.
    if (pin.empty() || candidateSignerDid.empty()) return false;
    return pin == candidateSignerDid;
}

bool PackageDownloaderLib::downloadedSignerBinds(bool fileSigned,
                                                 bool fileSignatureValid,
                                                 const std::string& fileSignerDid,
                                                 const std::string& advertisedDid) {
    // Stated as a conjunction of everything that must hold, for the same
    // reason signerPinMatches is stated positively: the predicate this
    // replaced was `fileSigned && fileDid == advDid`, and the missing term was
    // invisible precisely because the two present ones read as sufficient.
    //
    // An empty advertised DID never binds anything — callers only reach here
    // with a non-empty one, but the invariant belongs next to the comparison
    // so a caller constructed some other way cannot reopen it.
    if (advertisedDid.empty()) return false;
    if (!fileSigned) return false;
    // THE TERM THAT WAS MISSING. Without it, `fileSignerDid` is only what the
    // package says about itself.
    if (!fileSignatureValid) return false;
    return fileSignerDid == advertisedDid;
}

bool PackageDownloaderLib::outranks(const std::string& candidateVersion, const std::string& candidateDate,
                                    const std::string& incumbentVersion, const std::string& incumbentDate) {
    const int cmp = logos::semver::compare(candidateVersion, incumbentVersion);
    if (cmp != 0) return cmp > 0;
    return candidateDate > incumbentDate;  // same version: newest publish wins
}

} // namespace lgpd
