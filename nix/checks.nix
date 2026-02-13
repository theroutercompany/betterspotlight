{ pkgs }:

let
  repoRoot = ../.;
in
{
  ci-script-contract = pkgs.runCommand "betterspotlight-ci-script-contract" {
    nativeBuildInputs = [ pkgs.bash ];
  } ''
    set -euo pipefail
    test -x ${repoRoot}/scripts/ci/configure.sh
    test -x ${repoRoot}/scripts/ci/build.sh
    test -x ${repoRoot}/scripts/ci/test.sh
    test -x ${repoRoot}/scripts/ci/coverage.sh
    mkdir -p "$out"
    echo "ok" > "$out/result"
  '';
}
