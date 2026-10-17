#version 450

layout(set = 0, binding = 0)
uniform sampler2D s_image;

layout(push_constant)
uniform push_data {
  uvec2 extent;
} args;

layout(location = 0) out vec4 o_color;

const float FXAA_EDGE_THRESHOLD     = 1.0 / 8.0;
const float FXAA_EDGE_THRESHOLD_MIN = 1.0 / 16.0;
const float FXAA_REDUCE_MIN         = 1.0 / 128.0;
const float FXAA_REDUCE_MUL         = 1.0 / 8.0;
const float FXAA_SPAN_MAX           = 8.0;

float luma(vec3 c) {
  return dot(c, vec3(0.299, 0.587, 0.114));
}

void main() {
  vec2 rcp = 1.0 / vec2(args.extent);
  vec2 uv  = gl_FragCoord.xy * rcp;

  vec4 centre = texture(s_image, uv);

  vec3 rgbM  = centre.rgb;
  vec3 rgbNW = texture(s_image, uv + vec2(-1.0, -1.0) * rcp).rgb;
  vec3 rgbNE = texture(s_image, uv + vec2( 1.0, -1.0) * rcp).rgb;
  vec3 rgbSW = texture(s_image, uv + vec2(-1.0,  1.0) * rcp).rgb;
  vec3 rgbSE = texture(s_image, uv + vec2( 1.0,  1.0) * rcp).rgb;

  float lumaM  = luma(rgbM);
  float lumaNW = luma(rgbNW);
  float lumaNE = luma(rgbNE);
  float lumaSW = luma(rgbSW);
  float lumaSE = luma(rgbSE);

  float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
  float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

  if (lumaMax - lumaMin < max(FXAA_EDGE_THRESHOLD_MIN, lumaMax * FXAA_EDGE_THRESHOLD)) {
    o_color = centre;
    return;
  }

  vec2 dir;
  dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
  dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

  float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * FXAA_REDUCE_MUL), FXAA_REDUCE_MIN);
  float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);

  dir = clamp(dir * rcpDirMin, vec2(-FXAA_SPAN_MAX), vec2(FXAA_SPAN_MAX)) * rcp;

  vec3 rgbA = 0.5 * (texture(s_image, uv + dir * (1.0 / 3.0 - 0.5)).rgb
                   + texture(s_image, uv + dir * (2.0 / 3.0 - 0.5)).rgb);

  vec3 rgbB = rgbA * 0.5 + 0.25 * (texture(s_image, uv + dir * -0.5).rgb
                                 + texture(s_image, uv + dir *  0.5).rgb);

  float lumaB = luma(rgbB);

  vec3 result = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;

  o_color = vec4(result, centre.a);
}
