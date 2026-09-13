{
  description = "Logos Package Downloader - Online package catalog and download library";

  inputs = {
    logos-nix.url = "github:logos-co/logos-nix";
    nixpkgs.follows = "logos-nix/nixpkgs";
    # logos-package supplies the lgx C library — used post-download to
    # verify a fetched .lgx against what the catalog advertised.
    logos-package.url = "github:logos-co/logos-package";
    nix-bundle-dir.url = "github:logos-co/nix-bundle-dir";
    nix-bundle-appimage.url = "github:logos-co/nix-bundle-appimage";
  };

  outputs = { self, nixpkgs, logos-nix, logos-package, nix-bundle-dir, nix-bundle-appimage }:
    let
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];
      # Build info baked into the lgpd binary so `--version` reports the release
      # version, this repo's commit, and the locked commits of the flake inputs.
      # `revOf` yields the input's locked rev, a "<sha>-dirty" marker for a dirty
      # checkout, or "dirty" for a path override.
      revOf = input: input.rev or input.dirtyRev or "dirty";
      buildInfo = {
        # VERSION is only present on release branches. On master (pre-release CI
        # builds) there is no VERSION file, so fall back to a "pre-release-{sha7}"
        # string derived from self.rev. Dirty local builds lack self.rev and get
        # an empty string, which the CLI renders as "dev".
        version = if builtins.pathExists ./VERSION
          then nixpkgs.lib.removeSuffix "\n" (builtins.readFile ./VERSION)
          else if (self ? rev) then "pre-release-${builtins.substring 0 7 self.rev}" else "";
        commit = revOf self;
        commits = [
          { name = "logos-package"; commit = revOf logos-package; }
          { name = "logos-nix"; commit = revOf logos-nix; }
          { name = "nix-bundle-dir"; commit = revOf nix-bundle-dir; }
          { name = "nix-bundle-appimage"; commit = revOf nix-bundle-appimage; }
        ];
      };
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f {
        inherit system;
        pkgs = import nixpkgs { inherit system; };
        dirBundler = nix-bundle-dir.bundlers.${system}.permissive;
        logosPackageLib = logos-package.packages.${system}.lib;
      });
      # Adds the "x86_64-windows" pseudo-system. PACKAGES only -- `checks` cannot
      # run (ctest would have to execute PE binaries on the Linux build host)
      # and a cross devShell offers no way to run what it produces.
      forAllTargets = logos-nix.lib.forAllTargets;

      # The iOS targets, and the ONE build platform that can produce them
      # (Xcode). Android is absent for the same reason it is absent from
      # logos-package: lgx cross-compiles there as a SHARED object, and whoever
      # embeds it in an APK has to answer for liblgx.so being in the APK too.
      iosBuildSystem = "aarch64-darwin";
      iosTargets = [ "aarch64-ios" "aarch64-ios-simulator" ];
      mobileLibs = nixpkgs.lib.genAttrs iosTargets (target:
        let
          pkgs = logos-nix.lib.mkIosPkgs { inherit target; buildSystem = iosBuildSystem; };
          lgx = logos-package.legacyPackages.${iosBuildSystem}.mobile.${target}.lib;
        in
        {
          lib = import ./nix/mobile-ios.nix {
            inherit pkgs lgx;
            src = ./.;
            # A string; nothing else in the desktop common config is touched.
            inherit (import ./nix/default.nix { inherit pkgs; logosPackageLib = lgx; }) version;
          };
        });
    in
    {
      packages = forAllTargets ({ pkgs, system }:
        let
          logosPackageLib = logos-package.packages.${system}.lib;
          dirBundler = nix-bundle-dir.bundlers.${system}.permissive;
          # Windows exposes the bare CLI only. nix-bundle-dir has no PE backend --
          # and does not need one for a plain console tool: PE import tables
          # carry DLL BASE NAMES rather than paths (no rpath exists in the
          # format), Windows searches the executable's own directory first, and
          # nixpkgs' win-dll-link.sh already stages every dependency DLL there.
          # So a dereferencing copy of $out/bin is self-contained, and the path
          # rewriting the ELF/Mach-O bundlers exist to do has no analogue here.
          # Verified end to end: lgx.exe round-trips a package on a Windows box
          # with no Nix installed.
          isWindows = pkgs.stdenv.hostPlatform.isWindows;

          common = import ./nix/default.nix { inherit pkgs logosPackageLib; };
          src = ./.;

          lib = import ./nix/lib.nix { inherit pkgs common src logosPackageLib; };
          cli = import ./nix/cli.nix { inherit pkgs common src logosPackageLib buildInfo; };

          combined = pkgs.symlinkJoin {
            name = "logos-package-downloader";
            paths = [ lib cli ];
          };
        in
        {
          logos-package-downloader-lib = lib;
          logos-package-downloader-cli = cli;
          lib = lib;
          cli = cli;

        } // pkgs.lib.optionalAttrs (!isWindows) {
          cli-bundle-dir = dirBundler cli;
        } // pkgs.lib.optionalAttrs pkgs.stdenv.isLinux {
          cli-appimage = nix-bundle-appimage.lib.${system}.mkAppImage {
            drv = cli;
            name = "lgpd";
            bundle = dirBundler cli;
            desktopFile = ./assets/lgpd.desktop;
            icon = ./assets/lgpd.png;
          };
        } // pkgs.lib.optionalAttrs (!isWindows) {
          # Tests
          tests = import ./nix/tests.nix { inherit pkgs common src logosPackageLib; };
        } // {
          default = combined;
        }
      );

      # `legacyPackages`, not `packages`: a cross derivation's `system` is its
      # BUILD platform, so an iOS archive published under `packages` would
      # collide with the native aarch64-darwin set and `nix flake check` would
      # try to realise it as a Mac one. The shape is what
      # logos-module-builder's `mobilePackages` seam reads.
      legacyPackages.${iosBuildSystem}.mobile = mobileLibs;

      checks = forAllSystems ({ pkgs, logosPackageLib, ... }:
        let
          common = import ./nix/default.nix { inherit pkgs logosPackageLib; };
          src = ./.;
        in {
          tests = import ./nix/tests.nix { inherit pkgs common src logosPackageLib; };
        }
      );

      devShells = forAllSystems ({ pkgs, logosPackageLib, ... }: {
        default = pkgs.mkShell {
          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
          ];
          buildInputs = [
            pkgs.nlohmann_json
            pkgs.curl
            pkgs.zstd
            logosPackageLib
          ];

          shellHook = ''
            echo "Logos Package Downloader development environment"
          '';
        };
      });
    };
}
