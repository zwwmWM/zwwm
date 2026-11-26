{ lib, stdenv, cmake, ninja, pkg-config }:

stdenv.mkDerivation {
  pname = "zwayland-scanner";
  version = lib.strings.trim (builtins.readFile ../.version);

  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [ ../.version ../LICENSE ../zwayland-scanner ];
  };
  postUnpack = ''sourceRoot+=/zwayland-scanner'';

  nativeBuildInputs = [ cmake ninja pkg-config ];

  meta = {
    description = "Native C++23 Wayland client/server binding generator";
    homepage = "https://github.com/zwwmWM/zwwm";
    license = lib.licenses.bsd3;
    platforms = lib.platforms.linux;
    mainProgram = "zwayland-scanner";
  };
}
