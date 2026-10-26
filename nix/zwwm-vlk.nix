{ lib, stdenv, cmake, ninja, pkg-config, zwayland, vulkan-headers, vulkan-loader, libdrm }:

stdenv.mkDerivation {
  pname = "zwwm-vlk";
  version = lib.strings.trim (builtins.readFile ../.version);

  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [ ../.version ../LICENSE ../zwwm-vlk ];
  };
  postUnpack = ''sourceRoot+=/zwwm-vlk'';

  nativeBuildInputs = [ cmake ninja pkg-config ];
  buildInputs = [ libdrm ];
  propagatedBuildInputs = [ zwayland vulkan-headers vulkan-loader ];
  cmakeFlags = [ "-DBUILD_SHARED_LIBS=ON" ];

  meta = {
    description = "Vulkan device, DMA-BUF image, and presentation helpers shared by zwwm and zterm";
    homepage = "https://github.com/binarylinuxx/zwwm";
    license = lib.licenses.bsd3;
    platforms = lib.platforms.linux;
  };
}
