cbuffer GmsdConstants : register(b0) {
  uint width;
  uint height;
  uint group_count_x;
  uint reserved0;
};

StructuredBuffer<float> reference_luma : register(t0);
StructuredBuffer<float> candidate_luma : register(t1);
RWStructuredBuffer<float4> partials : register(u0);

groupshared float shared_sum[256];
groupshared float shared_sum_sq[256];
groupshared float shared_count[256];

uint image_index(uint x, uint y) {
  return y * width + x;
}

uint candidate_image_index(uint candidate_index, uint x, uint y) {
  return candidate_index * width * height + image_index(x, y);
}

float reference_at(uint x, uint y) {
  return reference_luma[image_index(min(x, width - 1u), min(y, height - 1u))];
}

float candidate_at(uint candidate_index, uint x, uint y) {
  return candidate_luma[candidate_image_index(candidate_index, min(x, width - 1u), min(y, height - 1u))];
}

float reference_gradient(uint x, uint y) {
  const uint left = x == 0u ? x : x - 1u;
  const uint right = min(x + 1u, width - 1u);
  const uint top = y == 0u ? y : y - 1u;
  const uint bottom = min(y + 1u, height - 1u);
  const float dx = reference_at(right, y) - reference_at(left, y);
  const float dy = reference_at(x, bottom) - reference_at(x, top);
  return sqrt(dx * dx + dy * dy);
}

float candidate_gradient(uint candidate_index, uint x, uint y) {
  const uint left = x == 0u ? x : x - 1u;
  const uint right = min(x + 1u, width - 1u);
  const uint top = y == 0u ? y : y - 1u;
  const uint bottom = min(y + 1u, height - 1u);
  const float dx = candidate_at(candidate_index, right, y) - candidate_at(candidate_index, left, y);
  const float dy = candidate_at(candidate_index, x, bottom) - candidate_at(candidate_index, x, top);
  return sqrt(dx * dx + dy * dy);
}

[numthreads(16, 16, 1)]
void main(uint3 group_id : SV_GroupID,
          uint3 group_thread_id : SV_GroupThreadID,
          uint3 dispatch_thread_id : SV_DispatchThreadID) {
  const uint local_index = group_thread_id.y * 16u + group_thread_id.x;
  float sum = 0.0f;
  float sum_sq = 0.0f;
  float count = 0.0f;

  if (dispatch_thread_id.x < width && dispatch_thread_id.y < height) {
    const float ref_grad = reference_gradient(dispatch_thread_id.x, dispatch_thread_id.y);
    const float candidate_grad = candidate_gradient(group_id.z, dispatch_thread_id.x, dispatch_thread_id.y);
    const float c = 0.0026f;
    const float similarity = (2.0f * ref_grad * candidate_grad + c) /
                             (ref_grad * ref_grad + candidate_grad * candidate_grad + c);
    sum = similarity;
    sum_sq = similarity * similarity;
    count = 1.0f;
  }

  shared_sum[local_index] = sum;
  shared_sum_sq[local_index] = sum_sq;
  shared_count[local_index] = count;
  GroupMemoryBarrierWithGroupSync();

  for (uint stride = 128u; stride > 0u; stride >>= 1u) {
    if (local_index < stride) {
      shared_sum[local_index] += shared_sum[local_index + stride];
      shared_sum_sq[local_index] += shared_sum_sq[local_index + stride];
      shared_count[local_index] += shared_count[local_index + stride];
    }
    GroupMemoryBarrierWithGroupSync();
  }

  if (local_index == 0u) {
    const uint partials_per_candidate = group_count_x * ((height + 15u) / 16u);
    partials[group_id.z * partials_per_candidate + group_id.y * group_count_x + group_id.x] =
        float4(shared_sum[0], shared_sum_sq[0], shared_count[0], 0.0f);
  }
}
