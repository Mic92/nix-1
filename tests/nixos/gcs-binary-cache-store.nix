{ config, ... }:

let
  pkgs = config.nodes.machine.nixpkgs.pkgs;

  pkgA = pkgs.writeText "gcs-test-pkg-a" "hello from gcs";

  token = "test-access-token";
  port = 4443;

  # authorized_user ADC file pointing at the mock's /token endpoint, so the
  # full refresh-token flow runs without needing RS256 signing.
  adcFile = pkgs.writeText "adc.json" (
    builtins.toJSON {
      type = "authorized_user";
      client_id = "dummy";
      client_secret = "dummy";
      refresh_token = "dummy";
      token_uri = "http://localhost:${toString port}/token";
    }
  );

  # external_account (workload identity federation): a file-sourced subject
  # token exchanged at the mock's STS endpoint. The second variant additionally
  # impersonates a service account.
  subjectTokenFile = pkgs.writeText "subject-token" "fake-oidc-subject-token";
  externalAccount = {
    type = "external_account";
    audience = "//iam.googleapis.com/projects/0/locations/global/workloadIdentityPools/p/providers/x";
    subject_token_type = "urn:ietf:params:oauth:token-type:jwt";
    token_url = "http://localhost:${toString port}/sts";
    credential_source.file = "${subjectTokenFile}";
  };
  wifAdc = pkgs.writeText "wif-adc.json" (builtins.toJSON externalAccount);
  wifImpersonateAdc = pkgs.writeText "wif-imp-adc.json" (
    builtins.toJSON (
      externalAccount
      // {
        service_account_impersonation_url = "http://localhost:${toString port}/v1/projects/-/serviceAccounts/sa@example.iam.gserviceaccount.com:generateAccessToken";
      }
    )
  );
in
{
  name = "gcs-binary-cache-store";

  nodes.machine =
    { pkgs, ... }:
    {
      virtualisation.writableStore = true;
      virtualisation.additionalPaths = [ pkgA ];
      nix.extraOptions = ''
        experimental-features = nix-command
        substituters =
      '';
      systemd.services.gcs-mock = {
        wantedBy = [ "multi-user.target" ];
        serviceConfig.ExecStart = "${pkgs.python3}/bin/python3 ${./gcs-mock-server.py} --port ${toString port} --token ${token} --public-bucket public-cache";
      };
    };

  testScript =
    { nodes }:
    # python
    ''
      PKG_A = "${pkgA}"
      ENV = "GOOGLE_APPLICATION_CREDENTIALS=${adcFile}"
      # Bare host:port (not a full URL) plus scheme=http: regression guard for
      # the endpoint-parsing fix. The mock speaks http, so scheme must override
      # the https default.
      ENDPOINT = "endpoint=localhost:${toString port}&scheme=http"

      def store_url(bucket, **extra):
          q = "&".join([ENDPOINT] + [f"{k}={v}" for k, v in extra.items()])
          return f"gs://{bucket}?{q}"

      machine.wait_for_unit("gcs-mock.service")
      machine.wait_for_open_port(${toString port})

      url = store_url("private-cache")

      # === auth is actually enforced (no creds → 401) ===
      out = machine.fail(f"nix store info --store '{url}' 2>&1")
      assert "401" in out, out

      # === push/pull round-trip with ADC auth ===
      machine.succeed(f"{ENV} nix copy --to '{url}' {PKG_A}")
      machine.succeed(f"nix store delete --ignore-liveness {PKG_A}")
      machine.fail(f"nix path-info {PKG_A}")
      machine.succeed(f"{ENV} nix copy --no-check-sigs --from '{url}' {PKG_A}")
      machine.succeed(f"nix path-info {PKG_A}")

      # === workload identity federation (external_account) ===
      # Direct STS token exchange: file subject-token source, no impersonation.
      WIF = "GOOGLE_APPLICATION_CREDENTIALS=${wifAdc}"
      machine.succeed(f"{WIF} nix copy --to '{url}' {PKG_A}")
      machine.succeed(f"nix store delete --ignore-liveness {PKG_A}")
      machine.succeed(f"{WIF} nix copy --no-check-sigs --from '{url}' {PKG_A}")
      machine.succeed(f"nix path-info {PKG_A}")

      # Federated token exchanged again for an impersonated service-account token.
      WIF_IMP = "GOOGLE_APPLICATION_CREDENTIALS=${wifImpersonateAdc}"
      machine.succeed(f"nix store delete --ignore-liveness {PKG_A}")
      machine.succeed(f"{WIF_IMP} nix copy --no-check-sigs --from '{url}' {PKG_A}")
      machine.succeed(f"nix path-info {PKG_A}")

      # === anonymous read from public bucket ===
      pub = store_url("public-cache")
      machine.succeed(f"{ENV} nix copy --to '{pub}' {PKG_A}")
      machine.succeed(f"nix store delete --ignore-liveness {PKG_A}")
      machine.succeed(f"nix copy --no-check-sigs --from '{pub}' {PKG_A}")
      machine.succeed(f"nix path-info {PKG_A}")

      # === builtin:fetchurl from a public bucket with NO credentials ===
      # Regression for the fork deadlock: the sandboxed child must not re-run
      # the ADC chain (which would block on the post-fork global FileTransfer).
      pub_info = f"gs://public-cache/nix-cache-info?{ENDPOINT}"
      pub_hash = machine.succeed(f"nix-prefetch-url '{pub_info}'").strip()
      machine.succeed(
          "nix build --impure --no-link --expr '"
          f'import <nix/fetchurl.nix> {{ name = "gcs-public-fork"; url = "{pub_info}"; sha256 = "{pub_hash}"; }}'
          "' 2>&1"
      )

      # === multipart upload through S3CompatBinaryCacheStore ===
      large = machine.succeed(
          "nix-store --add $(dd if=/dev/urandom of=/tmp/large bs=1M count=10 2>/dev/null && echo /tmp/large)"
      ).strip()
      mp_url = store_url(
          "private-cache",
          **{"multipart-upload": "true", "multipart-threshold": str(5 * 1024 * 1024)}
      )
      out = machine.succeed(f"{ENV} nix copy --debug --to '{mp_url}' {large} 2>&1")
      assert "using GCS multipart upload" in out, out
      assert "parts uploaded" in out, out
      machine.succeed(f"nix store delete --ignore-liveness {large}")
      machine.succeed(f"{ENV} nix copy --no-check-sigs --from '{mp_url}' {large}")
      machine.succeed(f"nix path-info {large}")

      # === builtin:fetchurl gs:// inside the sandbox ===
      # The forked builder has no ADC file or metadata server; the parent must
      # pre-resolve the bearer token and pass it via BuiltinBuilderContext.
      info_url = f"gs://private-cache/nix-cache-info?{ENDPOINT}"
      info_hash = machine.succeed(
          f"{ENV} nix-prefetch-url '{info_url}'"
      ).strip()
      out = machine.succeed(
          f"{ENV} nix build --debug --impure --no-link --print-out-paths --expr '"
          f'import <nix/fetchurl.nix> {{ name = "gcs-fork-test"; url = "{info_url}"; sha256 = "{info_hash}"; }}'
          "' 2>&1"
      )
      assert "Pre-resolving GCP access token" in out, out
      assert "Using pre-resolved GCP access token from parent process" in out, out

      # === user-project header forwarded ===
      up_url = store_url("billing-cache", **{"user-project": "billing-proj"})
      machine.succeed(f"{ENV} nix copy --to '{up_url}' {PKG_A}")
      machine.wait_until_succeeds(
          "journalctl -u gcs-mock --no-pager | grep -q user-project=billing-proj",
          timeout=30,
      )
    '';
}
