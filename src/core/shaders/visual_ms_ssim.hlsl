cbuffer SsimConstants : register(b0) {
  uint width;
  uint height;
  uint group_count_x;
  uint reserved0;
};

struct SsimPartial {
  float sum_ref;
  float sum_candidate;
  float sum_ref_sq;
  float sum_candidate_sq;
  float sum_cross;
  float count;
  float padding0;
  float padding1;
};

StructuredBuffer<float> reference_luma : register(t0);
StructuredBuffer<float> candidate_luma : register(t1);
RWStructuredBuffer<SsimPartial> partials : register(u0);

groupshared float shared_ref[256];
groupshared float shared_candidate[256];
groupshared float shared_ref_sq[256];
groupshared float shared_candidate_sq[256];
groupshared float shared_cross[256];
groupshared float shared_count[256];

[numthreads(16, 16, 1)]
void main(uint3 group_id : SV_GroupID,
          uint3 group_thread_id : SV_GroupThreadID,
          uint3 dispatch_thread_id : SV_DispatchThreadID) {
  const uint local_index = group_thread_id.y * 16u + group_thread_id.x;
  float ref_value = 0.0f;
  float candidate_value = 0.0f;
  float count = 0.0f;

  if (dispatch_thread_id.x < width && dispatch_thread_id.y < height) {
    const uint index = dispatch_thread_id.y * width + dispatch_thread_id.x;
    ref_value = reference_luma[index];
    candidate_value = candidate_luma[index];
    count = 1.0f;
  }

  shared_ref[local_index] = ref_value;
  shared_candidate[local_index] = candidate_value;
  shared_ref_sq[local_index] = ref_value * ref_value;
  shared_candidate_sq[local_index] = candidate_value * candidate_value;
  shared_cross[local_index] = ref_value * candidate_value;
  shared_count[local_index] = count;
  GroupMemoryBarrierWithGroupSync();

  for (uint stride = 128u; stride > 0u; stride >>= 1u) {
    if (local_index < stride) {
      shared_ref[local_index] += shared_ref[local_index + stride];
      shared_candidate[local_index] += shared_candidate[local_index + stride];
      shared_ref_sq[local_index] += shared_ref_sq[local_index + stride];
      shared_candidate_sq[local_index] += shared_candidate_sq[local_index + stride];
      shared_cross[local_index] += shared_cross[local_index + stride];
      shared_count[local_index] += shared_count[local_index + stride];
    }
    GroupMemoryBarrierWithGroupSync();
  }

  if (local_index == 0u) {
    SsimPartial partial;
    partial.sum_ref = shared_ref[0];
    partial.sum_candidate = shared_candidate[0];
    partial.sum_ref_sq = shared_ref_sq[0];
    partial.sum_candidate_sq = shared_candidate_sq[0];
    partial.sum_cross = shared_cross[0];
    partial.count = shared_count[0];
    partial.padding0 = 0.0f;
    partial.padding1 = 0.0f;
    partials[group_id.y * group_count_x + group_id.x] = partial;
  }
}
