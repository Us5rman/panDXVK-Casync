#version 450
#extension GL_GOOGLE_include_directive : enable

layout(set = 0, binding = 0)
uniform sampler2D s_color;

layout(push_constant)
uniform push_data {
  uvec2 extent;
} args;

layout(location = 0) out vec4 o_color;

#define SMAA_RT_METRICS vec4(1.0 / float(args.extent.x), 1.0 / float(args.extent.y), float(args.extent.x), float(args.extent.y))
#define SMAA_GLSL_4 1
#define SMAA_PRESET_HIGH 1
#include "SMAA.hlsl"

void main() {
  vec2 texcoord = gl_FragCoord.xy / vec2(args.extent);

  vec4 offset[3];
  SMAAEdgeDetectionVS(texcoord, offset);

  vec2 edges = SMAALumaEdgeDetectionPS(texcoord, offset, s_color);

  o_color = vec4(edges, 0.0, 1.0);
}
