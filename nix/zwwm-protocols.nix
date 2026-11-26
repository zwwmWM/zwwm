{ lib, stdenvNoCC, zwayland-scanner }:

stdenvNoCC.mkDerivation {
  pname = "zwwm-protocols";
  version = lib.strings.trim (builtins.readFile ../.version);

  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../.version ../LICENSE ../THIRD_PARTY.md
      ../zwwm-protocols/protocols
      ../zwwm-protocols/zwwm-protocols.pc.in
    ];
  };

  nativeBuildInputs = [ zwayland-scanner ];
  dontConfigure = true;
  dontBuild = true;
  installPhase = ''
    runHook preInstall
    mkdir -p "$out/share/zwwm-protocols" "$out/share/pkgconfig" "$out/share/doc/zwwm-protocols"
    cp -r zwwm-protocols/protocols/. "$out/share/zwwm-protocols/"
    # zxml sources also ship as Wayland XML for third-party generators.
    for zxml in zwwm-protocols/protocols/*.zxml; do
      zwayland-scanner xml "$zxml" "$out/share/zwwm-protocols/$(basename "$zxml" .zxml).xml"
    done
    cp LICENSE THIRD_PARTY.md "$out/share/doc/zwwm-protocols/"
    substitute zwwm-protocols/zwwm-protocols.pc.in "$out/share/pkgconfig/zwwm-protocols.pc" \
      --subst-var-by prefix "$out" --subst-var-by version "$version"
    runHook postInstall
  '';

  meta = {
    description = "zwwm and vendored upstream Wayland protocol XML for third-party generators";
    homepage = "https://github.com/zwwmWM/zwwm";
    license = with lib.licenses; [ bsd3 mit ];
    platforms = lib.platforms.all;
  };
}
