# package_downloader_lib for a phone: the lgpd library as ONE SELF-CONTAINED
# STATIC ARCHIVE per iOS target.
#
# WHY IT IS ONE ARCHIVE AND NOT FOUR. On a desktop this package installs
# libpackage_downloader_lib.dylib with liblgx.dylib beside it and libcurl
# reachable through the store; the dynamic loader resolves all of it, and a
# consumer links ONE name. iOS loads no dynamic library of its own, so the same
# consumer -- package_downloader_module, whose CMakeLists names exactly
# `package_downloader_lib` in EXTERNAL_LIBS -- would have to learn that this
# library speaks HTTP through curl and verifies packages through lgx, and name
# both plus OpenSSL on its own link line. That is this repo's knowledge leaking
# into a module that has no reason to hold it.
#
# So the archives are folded together here. What is left out is left out because
# it is the PLATFORM's and there is nothing to fold: libz and the CoreFoundation
# / Security frameworks ship in the iOS SDK, and a consumer names them the way
# it names any system library.
{ pkgs, src, version, lgx }:

let
  buildInputs = [
    lgx
    pkgs.curl
    # curl's own CMake config find_dependency(OpenSSL)s: its TLS backend on
    # iOS is OpenSSL (logos-nix's nix/ios/third-party.nix says why it is not
    # Apple's).
    pkgs.openssl
    pkgs.nlohmann_json
  ];
  # The archives folded into the installed library, in link order.
  vendored = [
    "${lgx}/lib/liblgx.a"
    "${pkgs.curl.out or pkgs.curl}/lib/libcurl.a"
    "${pkgs.openssl}/lib/libssl.a"
    "${pkgs.openssl}/lib/libcrypto.a"
  ];
in
pkgs.mkIosCmakeStage {
  pname = "logos-package-downloader-lib-ios";
  inherit src version buildInputs;
  cmakeFlags = [
    # An iOS toolchain puts find_package/find_library in root-only mode: being
    # on CMAKE_PREFIX_PATH is not enough, every input has to be named as a ROOT.
    "-DCMAKE_FIND_ROOT_PATH=${pkgs.lib.concatStringsSep ";" (map toString buildInputs)}"
    "-DOPENSSL_ROOT_DIR=${pkgs.openssl}"
    "-DLGX_ROOT=${lgx}"
    "-DLGPD_STATIC_LIB=ON"
  ];
  postInstall = ''
    "$(xcrun --find libtool)" -static -no_warning_for_no_symbols \
      -o libpackage_downloader_lib-merged.a \
      "$out/lib/libpackage_downloader_lib.a" ${pkgs.lib.escapeShellArgs vendored}
    mv libpackage_downloader_lib-merged.a "$out/lib/libpackage_downloader_lib.a"

    # lgx.h travels too: the module's generated code includes it, and on a
    # desktop it arrives the same way (nix/lib.nix stages liblgx beside us).
    cp ${lgx}/include/lgx.h $out/include/
  '';
}
