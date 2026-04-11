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
    qt6.qtbase
    qt6.qtdeclarative
    qt6.qttools
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

    qt_base="${pkgs.qt6.qtbase}"
    qt_declarative="${pkgs.qt6.qtdeclarative}"
    qt_tools="${pkgs.qt6.qttools}"
    qt_prefixes="''${qt_base}:''${qt_declarative}:''${qt_tools}"

    if [[ -n "''${CMAKE_PREFIX_PATH:-}" ]]; then
      export CMAKE_PREFIX_PATH="''${qt_prefixes}:''${CMAKE_PREFIX_PATH}"
    else
      export CMAKE_PREFIX_PATH="''${qt_prefixes}"
    fi

    export Qt6_DIR="''${qt_base}/lib/cmake/Qt6"
    export Qt6Qml_DIR="''${qt_declarative}/lib/cmake/Qt6Qml"
    export Qt6Quick_DIR="''${qt_declarative}/lib/cmake/Qt6Quick"
    export Qt6QuickControls2_DIR="''${qt_declarative}/lib/cmake/Qt6QuickControls2"
    export Qt6QuickTemplates2_DIR="''${qt_declarative}/lib/cmake/Qt6QuickTemplates2"
    export Qt6QmlTools_DIR="''${qt_declarative}/lib/cmake/Qt6QmlTools"

    export ONNXRuntime_INCLUDE_DIR="${pkgs.onnxruntime.dev}/include"
    export ONNXRuntime_LIBRARY="${pkgs.onnxruntime}/lib/libonnxruntime.dylib"

    if [[ -d "''${qt_base}/lib/qt-6/plugins" ]]; then
      export QT_PLUGIN_PATH="''${qt_base}/lib/qt-6/plugins''${QT_PLUGIN_PATH:+:''${QT_PLUGIN_PATH}}"
    fi
    if [[ -d "''${qt_declarative}/lib/qt-6/qml" ]]; then
      export QML_IMPORT_PATH="''${qt_declarative}/lib/qt-6/qml''${QML_IMPORT_PATH:+:''${QML_IMPORT_PATH}}"
      export QML2_IMPORT_PATH="''${qt_declarative}/lib/qt-6/qml''${QML2_IMPORT_PATH:+:''${QML2_IMPORT_PATH}}"
    fi

    echo "[betterspotlight] Nix dev shell active (${pkgs.stdenv.hostPlatform.system})"
  '';
}
