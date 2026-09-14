# package_downloader_lib for a phone: the lgpd library as ONE SELF-CONTAINED
# STATIC ARCHIVE for aarch64-android, the same shape ./mobile-ios.nix produces
# for the two iOS targets.
#
# WHY ONE ARCHIVE AND NOT FIVE. On a desktop this package installs
# libpackage_downloader_lib.so with liblgx.so beside it and libcurl reachable
# through the store; the dynamic loader resolves all of it and a consumer links
# ONE name. Inside an APK there is no such loader: logos-nix's DT_NEEDED gate
# lets a shipped .so name only what Android guarantees at the app's API level or
# what the app packages beside it, so every one of those sonames would have to
# be answered for by whoever builds the APK. Folding them in here means the
# consumer -- package_downloader_module, whose CMakeLists names exactly
# `package_downloader_lib` in EXTERNAL_LIBS -- still links one name and still
# does not have to know that this library speaks HTTP through curl and verifies
# packages through lgx.
#
# WHAT IS *NOT* FOLDED IN, and why nothing is missing:
#
#   zlib        already inside liblgx.a, which is folded in below
#               (logos-package's nix/mobile-android.nix explains why lgx carries
#               it rather than naming the NDK's libz.so).
#   libc++      the ONE shared library an APK in this stack always packages, and
#               the one soname the gate is told to allow by name.
#
# THE CURL BUILD IS DELIBERATELY SMALL. nixpkgs' default libcurl for this target
# pulls in krb5, libssh2, nghttp2/nghttp3/ngtcp2, brotli, zstd, libidn2 and
# libpsl -- every one of which would be folded into every module that links
# lgpd, to support a protocol this library never speaks. lgpd fetches a catalog
# index and a `.lgx` over HTTP(S) and nothing else.
{ pkgs, src, version, lgx }:

let
  inherit (pkgs) lib;
  buildPkgs = pkgs.pkgsBuildBuild;

  # Static, and named once: curl below is built against THIS OpenSSL, so the
  # archives folded in are the ones the code was compiled against.
  openssl = pkgs.openssl.override { static = true; };

  curl = (pkgs.curl.override {
    inherit openssl;
    opensslSupport = true;
    zlibSupport = true;
    brotliSupport = false;
    zstdSupport = false;
    gssSupport = false;
    http2Support = false;
    http3Support = false;
    idnSupport = false;
    ldapSupport = false;
    pslSupport = false;
    scpSupport = false;
    websocketSupport = false;
  }).overrideAttrs (old: {
    # Appending is enough: autoconf lets the LAST setting of an option win, so
    # nixpkgs' own flag list does not have to be matched by hand.
    configureFlags = (old.configureFlags or [ ]) ++ [ "--enable-static" "--disable-shared" ];
  });

  buildInputs = [
    lgx
    curl
    openssl
    pkgs.nlohmann_json
  ];

  # The archives folded into the installed library, in link order.
  vendored = [
    "${lgx}/lib/liblgx.a"
    "${lib.getLib curl}/lib/libcurl.a"
    "${lib.getLib openssl}/lib/libssl.a"
    "${lib.getLib openssl}/lib/libcrypto.a"
  ];
in
pkgs.stdenv.mkDerivation {
  pname = "logos-package-downloader-lib-android";
  inherit src version buildInputs;

  nativeBuildInputs = [ buildPkgs.cmake buildPkgs.ninja ];

  cmakeFlags = [
    "-GNinja"
    # A cross toolchain re-roots find_package/find_library at the target
    # sysroot; BOTH is what lets the store prefixes above be searched too.
    "-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH"
    "-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH"
    "-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH"
    "-DBUILD_SHARED_LIBS=OFF"
    "-DOPENSSL_ROOT_DIR=${lib.getDev openssl}"
    "-DLGX_ROOT=${lgx}"
    "-DLGPD_STATIC_LIB=ON"
    # No executables in an APK, and a static lgpd CLI would be the one thing
    # here needing curl's TLS tail on its own link line -- see LGPD_BUILD_CLI
    # in CMakeLists.txt.
    "-DLGPD_BUILD_CLI=OFF"
  ];

  postInstall = ''
    # An MRI script rather than `ar x`-and-re-archive: it keeps every member,
    # including two that happen to share a name, which is what the ELF linker
    # expects to resolve against.
    "$AR" -M <<EOF
create $out/lib/libpackage_downloader_lib-merged.a
addlib $out/lib/libpackage_downloader_lib.a
${lib.concatMapStringsSep "\n" (a: "addlib ${a}") vendored}
save
end
EOF
    mv $out/lib/libpackage_downloader_lib-merged.a $out/lib/libpackage_downloader_lib.a

    # lgx.h travels too: the module's generated code includes it, and on a
    # desktop it arrives the same way (nix/lib.nix stages liblgx beside us).
    cp ${lgx}/include/lgx.h $out/include/
  '';

  meta = {
    description = "lgpd as a self-contained static archive for aarch64-android";
    platforms = lib.platforms.aarch64;
  };
}
