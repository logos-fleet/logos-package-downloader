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

      # The three mobile targets, each under the build platform that can
      # produce it: iOS needs Xcode (aarch64-darwin, no choice), Android builds
      # from either member of logos-nix's androidBuildSystems -- and a cross
      # derivation's `system` is its BUILD platform, so it is published under
      # each of them rather than under a target key nothing can realise.
      #
      # ALL THREE ARE SELF-CONTAINED STATIC ARCHIVES. iOS loads no dynamic
      # library of its own; Android would take one, but logos-nix's DT_NEEDED
      # gate refuses an unbundled soname inside an APK. Same shape, two reasons
      # -- see nix/mobile-ios.nix and nix/mobile-android.nix.
      iosBuildSystem = "aarch64-darwin";
      iosTargets = [ "aarch64-ios" "aarch64-ios-simulator" ];
      # `version` is the only thing read out of the desktop common config, and
      # it is a string: nothing in it is instantiated for a phone.
      mobileLib = { file, pkgs, lgx }: {
        lib = import file {
          inherit pkgs lgx;
          src = ./.;
          inherit (import ./nix/default.nix { inherit pkgs; logosPackageLib = lgx; }) version;
        };
      };
      mobileLibs = nixpkgs.lib.genAttrs iosTargets (target:
        mobileLib {
          file = ./nix/mobile-ios.nix;
          pkgs = logos-nix.lib.mkIosPkgs { inherit target; buildSystem = iosBuildSystem; };
          lgx = logos-package.legacyPackages.${iosBuildSystem}.mobile.${target}.lib;
        });
      # The build platforms that can produce the Android archive. logos-nix owns
      # this list, and the workspace's `follows` supplies a pin that publishes it
      # -- but the ATTRIBUTE NAMES of `legacyPackages` have to be computable from
      # THIS repo's own lock as well (`ws test` evaluates the sub-repo flake with
      # no overrides at all), and that pin predates `lib.androidBuildSystems`. So
      # the list is read when it is there and spelled out when it is not; the
      # derivations under those names still come from logos-nix and still fail
      # loudly on a pin too old to build them, exactly as the iOS ones do.
      androidBuildSystems =
        logos-nix.lib.androidBuildSystems or [ "x86_64-linux" "aarch64-darwin" ];
      androidLibs = buildSystem: {
        aarch64-android = mobileLib {
          file = ./nix/mobile-android.nix;
          pkgs = logos-nix.lib.mkAndroidPkgs { inherit buildSystem; };
          lgx = logos-package.legacyPackages.${buildSystem}.mobile.aarch64-android.lib;
        };
      };
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
      legacyPackages =
        nixpkgs.lib.genAttrs androidBuildSystems
          (buildSystem: { mobile = androidLibs buildSystem; })
        // {
          # Merged rather than assigned: aarch64-darwin builds both Android and
          # iOS, and writing it twice would drop one of the two.
          ${iosBuildSystem}.mobile = mobileLibs // (androidLibs iosBuildSystem);
        };

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
