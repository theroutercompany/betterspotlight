{ pkgs }:

let
  configureScript = ../scripts/ci/configure.sh;
  buildScript = ../scripts/ci/build.sh;
  testScript = ../scripts/ci/test.sh;
  coverageScript = ../scripts/ci/coverage.sh;
  verifyPipelineScript = ../scripts/dev/verify_pipeline.sh;
  verifyPreflightScript = ../scripts/dev/verify_preflight.py;
in
{
  ci-script-contract = pkgs.runCommand "betterspotlight-ci-script-contract" {
    nativeBuildInputs = [ pkgs.bash ];
  } ''
    set -euo pipefail
    test -x ${configureScript}
    test -x ${buildScript}
    test -x ${testScript}
    test -x ${coverageScript}
    test -x ${verifyPipelineScript}
    test -x ${verifyPreflightScript}
    mkdir -p "$out"
    echo "ok" > "$out/result"
  '';
}
