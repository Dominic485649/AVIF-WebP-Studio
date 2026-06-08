cbuffer LumaConstants : register(b0) {
  uint width;
  uint height;
  uint stride;
  uint channels;
};

StructuredBuffer<uint> input_words : register(t0);
RWStructuredBuffer<float> output_luma : register(u0);

uint load_byte(uint offset) {
  const uint word = input_words[offset >> 2];
  return (word >> ((offset & 3u) * 8u)) & 255u;
}

[numthreads(16, 16, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= width || id.y >= height) {
    return;
  }

  const uint input_byte_count = (height - 1u) * stride + width * channels;
  const uint input_word_stride = (input_byte_count + 3u) >> 2;
  const uint input_base = id.z * input_word_stride * 4u;
  const uint output_base = id.z * width * height;
  const uint base = input_base + id.y * stride + id.x * channels;
  const float r = (float)load_byte(base + 0u);
  const float g = (float)load_byte(base + 1u);
  const float b = (float)load_byte(base + 2u);
  output_luma[output_base + id.y * width + id.x] = (0.2126f * r + 0.7152f * g + 0.0722f * b) / 255.0f;
}
