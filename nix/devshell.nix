{ pkgs }:

pkgs.mkShell {
  packages = with pkgs; [
    bash
    cmake
    ninja
    pkg-config
    git
    jq
    python3
    clang
    llvmPackages_18.llvm
    llvmPackages_18.bintools
    qt6.full
    poppler
    tesseract
    leptonica
    onnxruntime
  ];

  shellHook = ''
    export BETTERSPOTLIGHT_NIX_SHELL=1

    export LC_ALL="''${LC_ALL:-C}"
    export LANG="''${LANG:-C}"
    export TZ="''${TZ:-UTC}"

    if [[ -z "''${SOURCE_DATE_EPOCH:-}" ]]; then
      SOURCE_DATE_EPOCH="$(git -C "''${PWD}" log -1 --pretty=%ct 2>/dev/null || date +%s)"
      export SOURCE_DATE_EPOCH
    fi

    qt_prefix="${pkgs.qt6.full}"
    if [[ -n "''${CMAKE_PREFIX_PATH:-}" ]]; then
      export CMAKE_PREFIX_PATH="''${qt_prefix}:''${CMAKE_PREFIX_PATH}"
    else
      export CMAKE_PREFIX_PATH="''${qt_prefix}"
    fi

    if [[ -d "${pkgs.qt6.full}/lib/qt-6/plugins" ]]; then
      export QT_PLUGIN_PATH="${pkgs.qt6.full}/lib/qt-6/plugins''${QT_PLUGIN_PATH:+:''${QT_PLUGIN_PATH}}"
    fi

    echo "[betterspotlight] Nix dev shell active (${pkgs.stdenv.hostPlatform.system})"
  '';
}
