#version 450
#extension GL_GOOGLE_include_directive : enable

layout(set = 0, binding = 0)
uniform sampler2D s_image;

layout(push_constant)
uniform push_data {
  uvec2 src_extent;
  uvec2 dst_extent;
  float rcas_stops;
} args;

layout(location = 0) out vec4 o_color;

#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"

#define FSR_EASU_F 1

AF4 FsrEasuRF(AF2 p) { return AF4(textureGather(s_image, p, 0)); }
AF4 FsrEasuGF(AF2 p) { return AF4(textureGather(s_image, p, 1)); }
AF4 FsrEasuBF(AF2 p) { return AF4(textureGather(s_image, p, 2)); }

#include "ffx_fsr1.h"

void main() {
  AU4 con0;
  AU4 con1;
  AU4 con2;
  AU4 con3;

  FsrEasuCon(con0, con1, con2, con3,
    AF1(args.src_extent.x), AF1(args.src_extent.y),
    AF1(args.src_extent.x), AF1(args.src_extent.y),
    AF1(args.dst_extent.x), AF1(args.dst_extent.y));

  AF3 color;
  FsrEasuF(color, AU2(gl_FragCoord.xy), con0, con1, con2, con3);

  o_color = vec4(color, 1.0);
}
