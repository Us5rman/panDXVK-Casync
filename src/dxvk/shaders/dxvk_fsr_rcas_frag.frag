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

#define FSR_RCAS_F 1

AF4 FsrRcasLoadF(ASU2 p) {
  ASU2 hi = ASU2(textureSize(s_image, 0)) - ASU2(1);
  return AF4(texelFetch(s_image, clamp(p, ASU2(0), hi), 0));
}

void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) { }

#include "ffx_fsr1.h"

void main() {
  AU4 con;
  FsrRcasCon(con, args.rcas_stops);

  AF1 r;
  AF1 g;
  AF1 b;
  FsrRcasF(r, g, b, AU2(gl_FragCoord.xy), con);

  o_color = vec4(r, g, b, 1.0);
}
