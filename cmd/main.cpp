// `lgpd` — Logos Package Downloader CLI.
//
// Commands:
//   list, search, info, download   — catalog operations.
//   repo {add,remove,list,enable,disable}
//                                  — manage the user repository config.
//   config {init,show,path}        — inspect / scaffold the config file.
//
// All `repo` mutating commands require `--config <path>`. The catalog is
// always available (the default repository is built into the downloader).

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "package_downloader_lib.h"
#include "version_info.h"

using json = nlohmann::json;

namespace {

struct CliOpts {
    std::string configPath;     // --config
    std::string repo;           // --repo (URL or name)
    std::string version;        // --version
    std::string rootHash;       // --root-hash
    std::string category;       // --category
    std::string outputDir;      // -o / --output
    bool jsonOutput = false;    // --json
};

void printGlobalHelp() {
    std::cout <<
R"HELP(lgpd — Logos Package Downloader

Usage: lgpd [global options] <command> [arguments]

Catalog commands:
  list                          List all available packages
  search <query>                Search packages by name or description
  info <package>                Show package details (all versions, sorted by date)
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
  config path                   Print the current --config path (or "(none)")

Global options:
  --config <path>               Path to repositories.json (required for any
                                mutating repo command).
  --repo <url-or-name>          Restrict a catalog/download command to one repo.
  --version <ver>               Pin a specific package version (download/info).
  --root-hash <hex>             Disambiguate two releases sharing a version.
  --category <cat>              Filter by category (list).
  -o, --output <dir>            Output directory (download).
  --json                        Emit structured JSON.
  -h, --help                    Show this help.
  -V, --version                 Print version (a bare --version, with no value
                                following, also prints the version).
)HELP";
}

int printVersion() {
    std::cout << lgpd_version::versionString() << "\n";
    return 0;
}

void truncate(std::string& s, size_t n) {
    if (s.size() > n) s = s.substr(0, n);
}

void printPackageTable(const json& packages) {
    std::printf("%-32s %-12s %-10s %-40s\n", "NAME", "CATEGORY", "TYPE", "DESCRIPTION");
    std::cout << std::string(98, '-') << "\n";
    for (const auto& pkg : packages) {
        std::string desc = pkg.value("description", ""); truncate(desc, 40);
        std::printf("%-32s %-12s %-10s %-40s\n",
            pkg.value("name", "").c_str(),
            pkg.value("category", "").c_str(),
            pkg.value("type", "").c_str(),
            desc.c_str());
    }
}

std::unique_ptr<lgpd::PackageDownloaderLib> makeLib(const CliOpts& o) {
    if (o.configPath.empty())
        return std::make_unique<lgpd::PackageDownloaderLib>();
    return std::make_unique<lgpd::PackageDownloaderLib>(o.configPath);
}

int cmdList(const CliOpts& o) {
    auto lib = makeLib(o);
    std::string body = o.repo.empty()
        ? lib->getCatalogJson()
        : lib->getCatalogForRepoJson(o.repo);
    json packages;
    try { packages = json::parse(body); } catch (...) {
        std::cerr << "Error: catalog parse failed\n"; return 1;
    }
    if (!o.category.empty()) {
        json filtered = json::array();
        std::string want = o.category;
        std::transform(want.begin(), want.end(), want.begin(), ::tolower);
        for (const auto& p : packages) {
            std::string c = p.value("category", "");
            std::transform(c.begin(), c.end(), c.begin(), ::tolower);
            if (c == want) filtered.push_back(p);
        }
        packages = std::move(filtered);
    }
    if (packages.empty()) { std::cout << "No packages found\n"; return 0; }
    if (o.jsonOutput) { std::cout << packages.dump(2) << "\n"; return 0; }
    std::cout << "Found " << packages.size() << " package(s):\n\n";
    printPackageTable(packages);
    return 0;
}

int cmdSearch(const CliOpts& o, const std::string& query) {
    auto lib = makeLib(o);
    json packages;
    // Honour --repo, same as cmdInfo: scope the search to one repo when
    // the user named one, otherwise search the merged catalog.
    try { packages = json::parse(
        o.repo.empty() ? lib->getCatalogJson() : lib->getCatalogForRepoJson(o.repo));
    } catch (...) {
        std::cerr << "Error: catalog parse failed\n"; return 1;
    }
    std::string q = query;
    std::transform(q.begin(), q.end(), q.begin(), ::tolower);
    json hits = json::array();
    for (const auto& p : packages) {
        std::string n = p.value("name", ""); std::string d = p.value("description", "");
        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        std::transform(d.begin(), d.end(), d.begin(), ::tolower);
        if (n.find(q) != std::string::npos || d.find(q) != std::string::npos)
            hits.push_back(p);
    }
    if (hits.empty()) { std::cout << "No packages found matching '" << query << "'\n"; return 0; }
    if (o.jsonOutput) { std::cout << hits.dump(2) << "\n"; return 0; }
    std::cout << "Found " << hits.size() << " package(s):\n\n";
    printPackageTable(hits);
    return 0;
}

int cmdInfo(const CliOpts& o, const std::string& name) {
    auto lib = makeLib(o);
    json packages;
    try { packages = json::parse(
        o.repo.empty() ? lib->getCatalogJson() : lib->getCatalogForRepoJson(o.repo));
    } catch (...) { std::cerr << "Error: catalog parse failed\n"; return 1; }
    json found;
    for (const auto& p : packages) {
        if (p.value("name", "") == name) { found = p; break; }
    }
    if (found.is_null()) { std::cerr << "Error: package '" << name << "' not found\n"; return 1; }
    if (o.jsonOutput) { std::cout << found.dump(2) << "\n"; return 0; }
    std::cout << "Name:        " << found.value("name", "") << "\n";
    std::cout << "Display:     " << found.value("displayName", "") << "\n";
    std::cout << "Type:        " << found.value("type", "") << "\n";
    std::cout << "Category:    " << found.value("category", "") << "\n";
    std::cout << "Author:      " << found.value("author", "") << "\n";
    std::cout << "Repository:  " << found.value("repositoryDisplayName", "")
              << " (" << found.value("repositoryUrl", "") << ")\n";
    std::cout << "Description: " << found.value("description", "") << "\n";
    // The variants the newest version ships: whether this package runs on the
    // reader's device, before anything is downloaded.
    if (found.contains("variants") && found["variants"].is_array()) {
        std::string variants;
        for (const auto& v : found["variants"]) {
            if (!variants.empty()) variants += ", ";
            variants += v.get<std::string>();
        }
        std::cout << "Variants:    " << (variants.empty() ? "(none)" : variants) << "\n";
    }
    std::cout << "\nVersions (newest first):\n";
    if (!found.contains("versions") || found["versions"].empty()) {
        std::cout << "  (none)\n";
        return 0;
    }
    for (const auto& v : found["versions"]) {
        // Null-safe: a version row can legitimately carry `manifest: null`
        // (early catalog rows). `v.value("manifest", json::object())`
        // returns the null when the key exists-but-null, and a chained
        // .value() on null throws — so guard for an actual object.
        const json manifest = (v.is_object() && v.contains("manifest")
                               && v["manifest"].is_object())
                              ? v["manifest"] : json::object();
        std::string ver = manifest.value("version", "");
        std::string date = v.value("releasedAt", "");
        std::string hash = v.value("rootHash", "");
        std::string shortHash = hash.size() > 12 ? (hash.substr(0, 8) + "..." + hash.substr(hash.size() - 4)) : hash;
        const bool signed_ = v.contains("signature") && v["signature"].is_object();
        std::cout << "  " << ver
                  << "  " << date
                  << "  " << shortHash
                  << "  " << (signed_ ? "[signed]" : "[unsigned]")
                  << "\n";
    }
    return 0;
}

int cmdDownload(const CliOpts& o, const std::string& name) {
    auto lib = makeLib(o);
    std::cout << "Downloading " << name;
    if (!o.version.empty()) std::cout << " v" << o.version;

    std::cout << "..." << std::flush;
    std::string err;
    std::string path =
        lib->downloadPackage(o.repo, name, err, o.version, o.rootHash, o.outputDir);

    if (path.empty()) {
        std::cout << " FAILED\n";
        std::cerr << "lgpd: " << err << "\n";
        return 1;
    }

    std::cout << " done\n  -> " << path << "\n";
    return 0;
}

int cmdRepoList(const CliOpts& o) {
    auto lib = makeLib(o);
    json repos;
    try { repos = json::parse(lib->listRepositoriesJson()); }
    catch (...) { std::cerr << "Error: failed to parse repo list\n"; return 1; }
    if (o.jsonOutput) { std::cout << repos.dump(2) << "\n"; return 0; }
    std::cout << "Configured repositories:\n";
    for (const auto& r : repos) {
        std::string tag;
        if (r.value("isDefault", false)) tag += " [default]";
        if (!r.value("enabled", true)) tag += " [disabled]";
        if (!r.value("resolveError", "").empty()) tag += " [error: " + r.value("resolveError", "") + "]";
        std::cout << "  " << r.value("name", "<unresolved>")
                  << "  (" << r.value("displayName", "") << ")"
                  << tag << "\n"
                  << "    url:      " << r.value("url", "") << "\n";
        if (!r.value("indexUrl", "").empty())
            std::cout << "    indexUrl: " << r.value("indexUrl", "") << "\n";
    }
    return 0;
}

int requireConfig(const CliOpts& o, const std::string& cmd) {
    if (!o.configPath.empty()) return 0;
    std::cerr << "Error: '" << cmd << "' requires --config <path>\n"
              << "Hint: run `lgpd config init /path/to/repositories.json` first,\n"
              << "      then pass --config /path/to/repositories.json to all repo commands.\n";
    return 1;
}

int cmdRepoMutate(const CliOpts& o, const std::string& action, const std::string& url) {
    if (int rc = requireConfig(o, "repo " + action); rc != 0) return rc;
    auto lib = makeLib(o);
    std::string err;
    if (action == "add") err = lib->registry().addRepository(url);
    else if (action == "remove") err = lib->registry().removeRepository(url);
    else if (action == "enable") err = lib->registry().setEnabled(url, true);
    else if (action == "disable") err = lib->registry().setEnabled(url, false);
    else { std::cerr << "Internal error: unknown action '" << action << "'\n"; return 1; }
    if (!err.empty()) { std::cerr << "Error: " << err << "\n"; return 1; }
    std::cout << "OK\n";
    return 0;
}

int cmdRepoRefresh(const CliOpts& o) {
    auto lib = makeLib(o);
    std::string err = lib->refreshCatalogs();
    if (!err.empty()) std::cerr << err;
    std::cout << "Refreshed.\n";
    return 0;
}

int cmdConfigInit(const std::string& path) {
    std::ofstream f(path);
    if (!f.is_open()) {
        std::cerr << "Error: cannot write " << path << "\n";
        return 1;
    }
    f << "{\n  \"schemaVersion\": 1,\n  \"repositories\": [],\n  \"defaultDisabled\": false,\n  \"defaultRemoved\": false\n}\n";
    if (!f.good()) { std::cerr << "Error: write failed: " << path << "\n"; return 1; }
    std::cout << "Created " << path << "\n";
    return 0;
}

int cmdConfigShow(const CliOpts& o) {
    if (o.configPath.empty()) {
        std::cerr << "Error: 'config show' requires --config <path>\n"; return 1;
    }
    std::ifstream f(o.configPath);
    if (!f.is_open()) {
        std::cerr << "Error: cannot read " << o.configPath << "\n"; return 1;
    }
    std::cout << f.rdbuf();
    return 0;
}

int cmdConfigPath(const CliOpts& o) {
    std::cout << (o.configPath.empty() ? std::string("(none)") : o.configPath) << "\n";
    return 0;
}

int dispatch(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    CliOpts o;
    std::vector<std::string> positional;

    for (size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "-h" || a == "--help") { printGlobalHelp(); return 0; }
        if (a == "-V") return printVersion();
        if (a == "--config" && i + 1 < args.size()) { o.configPath = args[++i]; continue; }
        if (a == "--repo" && i + 1 < args.size())   { o.repo = args[++i]; continue; }
        if (a == "--version") {
            // `--version <ver>` pins a package version (download/info); a bare
            // `--version` with no value following is treated like `-V` and prints
            // the tool version.
            if (i + 1 < args.size()) { o.version = args[++i]; continue; }
            return printVersion();
        }
        if (a == "--root-hash" && i + 1 < args.size()) { o.rootHash = args[++i]; continue; }
        if (a == "--category" && i + 1 < args.size()) { o.category = args[++i]; continue; }
        if ((a == "-o" || a == "--output") && i + 1 < args.size()) { o.outputDir = args[++i]; continue; }
        if (a == "--json") { o.jsonOutput = true; continue; }
        positional.push_back(a);
    }

    if (positional.empty()) { printGlobalHelp(); return 1; }
    std::string cmd = positional[0];
    positional.erase(positional.begin());

    if (cmd == "list")       return cmdList(o);
    if (cmd == "search") {
        if (positional.empty()) { std::cerr << "Error: search requires a query\n"; return 1; }
        return cmdSearch(o, positional[0]);
    }
    if (cmd == "info") {
        if (positional.empty()) { std::cerr << "Error: info requires a package name\n"; return 1; }
        return cmdInfo(o, positional[0]);
    }
    if (cmd == "download") {
        if (positional.empty()) { std::cerr << "Error: download requires a package name\n"; return 1; }
        return cmdDownload(o, positional[0]);
    }

    if (cmd == "repo") {
        if (positional.empty()) { std::cerr << "Error: repo requires a subcommand\n"; return 1; }
        std::string sub = positional[0]; positional.erase(positional.begin());
        if (sub == "list") return cmdRepoList(o);
        if (sub == "refresh") return cmdRepoRefresh(o);
        if (sub == "add" || sub == "remove" || sub == "enable" || sub == "disable") {
            if (positional.empty()) { std::cerr << "Error: repo " << sub << " requires a URL\n"; return 1; }
            return cmdRepoMutate(o, sub, positional[0]);
        }
        std::cerr << "Error: unknown repo subcommand '" << sub << "'\n"; return 1;
    }

    if (cmd == "config") {
        if (positional.empty()) { std::cerr << "Error: config requires a subcommand\n"; return 1; }
        std::string sub = positional[0]; positional.erase(positional.begin());
        if (sub == "init") {
            if (positional.empty()) { std::cerr << "Error: config init requires a path\n"; return 1; }
            return cmdConfigInit(positional[0]);
        }
        if (sub == "show") return cmdConfigShow(o);
        if (sub == "path") return cmdConfigPath(o);
        std::cerr << "Error: unknown config subcommand '" << sub << "'\n"; return 1;
    }

    std::cerr << "Error: unknown command '" << cmd << "'\n";
    std::cerr << "Run `lgpd --help` for usage.\n";
    return 1;
}

} // namespace

int main(int argc, char* argv[]) { return dispatch(argc, argv); }
