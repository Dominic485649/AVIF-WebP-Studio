cbuffer DownsampleConstants : register(b0) {
  uint source_width;
  uint source_height;
  uint output_width;
  uint output_height;
};

StructuredBuffer<float> source_luma : register(t0);
RWStructuredBuffer<float> output_luma : register(u0);

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= output_width || id.y >= output_height) {
    return;
  }

  const uint sx = id.x * 2u;
  const uint sy = id.y * 2u;
  const uint source_base = id.z * source_width * source_height;
  const uint output_base = id.z * output_width * output_height;
  float sum = 0.0f;
  float count = 0.0f;
  for (uint oy = 0u; oy < 2u && sy + oy < source_height; ++oy) {
    for (uint ox = 0u; ox < 2u && sx + ox < source_width; ++ox) {
      sum += source_luma[source_base + (sy + oy) * source_width + sx + ox];
      count += 1.0f;
    }
  }
  output_luma[output_base + id.y * output_width + id.x] = sum / count;
}
