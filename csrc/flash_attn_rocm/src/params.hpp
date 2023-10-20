// BSD 3 Clause
// Copyright 2023 Advanced Micro Devices, Inc.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
// 1. Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
// 3. Neither the name of the copyright holder nor the names of its contributors
// may be used to endorse or promote products derived from this software without
// specific prior written permission. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT
// HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
// INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
// FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
// OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
// EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


#pragma once

#include <vector>
#include <memory>

#include "utils.hpp"

// TODO: Use shared_ptr to use the same memory of BaseParams when calling forward/backward parameters
// Common argements used by both batched & grouped gemms
struct BaseParams {
  explicit BaseParams(const Index b,
                      const Index max_seqlen_q,
                      const Index max_seqlen_kv,
                      const Index h_q,
                      const Index h_kv,
                      const Index d,
                      const torch::Tensor &q,
                      const torch::Tensor &k,
                      const torch::Tensor &v,
                      torch::Tensor &out,
                      const float p_dropout,
                      const float softmax_scale,
                      const bool is_causal,
                      const bool z_permute)
    : b(b),
      max_seqlen_q(max_seqlen_q),
      max_seqlen_kv(max_seqlen_kv),
      h_q(h_q),
      h_kv(h_kv),
      d(d),
      p_dropout(p_dropout),
      softmax_scale(softmax_scale),
      is_bf16(q.dtype() == torch::kBFloat16),
      is_dropout(p_dropout > 0.0f),
      is_mnko_padding(false),
      is_causal(is_causal),
      z_permute(z_permute),
      q_seq_stride(q.stride(-3)),
      kv_seq_stride(k.stride(-3)),
      out_seq_stride(out.stride(-3)),
      q_head_stride(q.stride(-2)),
      kv_head_stride(k.stride(-2)),
      out_head_stride(out.stride(-2)) {

    TORCH_CHECK(p_dropout < 1.f);
    
    if(!is_mnko_padding && d <= 32) {
      is_mnko_padding = ((d % 32)==0 ? false : true);
    } else if(!is_mnko_padding && d <= 64) {
      is_mnko_padding = ((d % 64)==0 ? false : true);
    } else if(!is_mnko_padding && d <= 128) {
      is_mnko_padding = ((d % 128)==0 ? false : true);
    } else {
      std::cout << "Unsupported head dimension" << std::endl;
    }
  }  
  // The dimensions.
  Index b, max_seqlen_q, max_seqlen_kv, d;

  // The number of heads.
  Index h_q, h_kv;

  // The scaling factors for the kernel.
  float softmax_scale;
  // float softmax_scale_log2;

  // The dropout probability (probability of keeping an activation).
  float p_dropout;
  // uint8_t p_dropout_in_uint8_t;

  // Scale factor of 1 / (1 - p_dropout).
  // float rp_dropout;
  // float scale_softmax_rp_dropout;

  // Random state.
  at::PhiloxCudaState philox_args;

  // seeds
  std::tuple<uint64_t, uint64_t> seeds;

  // Pointer to the RNG seed (idx 0) and offset (idx 1).
  uint64_t* rng_state;

  bool is_bf16;
  bool is_dropout;
  bool is_mnko_padding;
  bool is_causal;

  bool z_permute;

  Index q_seq_stride;
  Index kv_seq_stride;
  Index out_seq_stride;

  Index q_head_stride;
  Index kv_head_stride;
  Index out_head_stride;

  static inline const bool kIsUnitTestMode = get_env_("FLASH_ATTENTION_INTERNAL_UNIT_TEST_MODE");
  static inline const bool kIsDeterministic = get_env_("FLASH_ATTENTION_INTERNAL_DETERMINISTIC");
};

// Common Batched Arguments
struct BatchedParams : public BaseParams {
  explicit BatchedParams(const Index b,
                         const Index max_seqlen_q,
                         const Index max_seqlen_kv,
                         const Index h_q,
                         const Index h_kv,
                         const Index d,
                         const torch::Tensor &q,
                         const torch::Tensor &k,
                         const torch::Tensor &v,
                         torch::Tensor &out,
                         void* z_d,
                         void* softmax_lse_d,
                         float p_dropout,
                         float softmax_scale,
                         bool is_causal,
                         bool z_permute)
    : BaseParams(b,
                 max_seqlen_q,
                 max_seqlen_kv,
                 h_q,
                 h_kv,
                 d,
                 q,
                 k,
                 v,
                 out,
                 p_dropout,
                 softmax_scale,
                 is_causal,
                 z_permute),
      q_ptr(q.data_ptr()),
      k_ptr(k.data_ptr()),
      v_ptr(v.data_ptr()),
      out_ptr(out.data_ptr()),
      z_ptr(z_d),
      softmax_lse_ptr(softmax_lse_d),
      q_batch_stride(q.stride(0)),
      kv_batch_stride(k.stride(0)),
      out_batch_stride(out.stride(0)) {
    
    if(!is_mnko_padding && d <= 32) {
      is_mnko_padding = ((max_seqlen_q % 128)==0 && (max_seqlen_kv % 128)==0 ? false : true);
    } else if(!is_mnko_padding && d <= 64) {
      if(is_dropout) {
        is_mnko_padding = ((max_seqlen_q % 128)==0 && (max_seqlen_kv % 128)==0 ? false : true);
      } else {
        is_mnko_padding = ((max_seqlen_q % 128)==0 && (max_seqlen_kv % 256)==0 ? false : true);
      }
    } else if(!is_mnko_padding && d <= 128) {
      is_mnko_padding = ((max_seqlen_q % 128)==0 && (max_seqlen_kv % 128)==0 ? false : true);
    }

    // Q layout [b, max_seqlen_q, h_q, d]
    q_lengths = std::vector<Index>{b, h_q, max_seqlen_q, d};
    q_strides = std::vector<Index>{q_batch_stride, q_head_stride, q_seq_stride, 1};

    // K layout [b, max_seqlen_kv, h_kv, d]
    k_lengths = std::vector<Index>{b, h_kv, max_seqlen_kv, d};
    k_strides = std::vector<Index>{kv_batch_stride, kv_head_stride, kv_seq_stride, 1};

    // V layout [b, max_seqlen_kv, h_kv, d]
    v_lengths = std::vector<Index>{b, h_kv, d, max_seqlen_kv};
    v_strides = std::vector<Index>{kv_batch_stride, kv_head_stride, 1, kv_seq_stride};

    // Y layout [b, max_seqlen_q, h_q, d]
    out_lengths = std::vector<Index>{b, h_q, max_seqlen_q, d};
    out_strides = std::vector<Index>{out_batch_stride, out_head_stride, out_seq_stride, 1};

    z_lengths = std::vector<Index>{b, h_q, max_seqlen_q, max_seqlen_kv};
    z_strides = 
        z_permute ? 
        std::vector<Index>{h_q*max_seqlen_q*max_seqlen_kv, max_seqlen_kv, h_q*max_seqlen_kv, 1} :
        // Z layout [b, max_seqlen_q, h_q, max_seqlen_kv]
        std::vector<Index>{h_q*max_seqlen_q*max_seqlen_kv, max_seqlen_q*max_seqlen_kv, max_seqlen_kv, 1};
        // Z layout [b, h_q, max_seqlen_q, max_seqlen_kv]

    // LSE layout [b, h_q, max_seqlen_q]
    lse_lengths = std::vector<Index>{b, h_q, max_seqlen_q};
    // std::vector<Index> lse_strides{h_q*max_seqlen_q, max_seqlen_q, 1};
  }
  
  void* __restrict__ q_ptr;
  void* __restrict__ k_ptr;
  void* __restrict__ v_ptr;

  void* __restrict__ out_ptr;
  void* __restrict__ z_ptr;
  void* __restrict__ softmax_lse_ptr;

  int q_batch_stride;
  int kv_batch_stride;
  int out_batch_stride;

  std::vector<Index> q_lengths;
  std::vector<Index> q_strides;
  std::vector<Index> k_lengths;
  std::vector<Index> k_strides;
  std::vector<Index> v_lengths;
  std::vector<Index> v_strides;
  std::vector<Index> out_lengths;
  std::vector<Index> out_strides;
  std::vector<Index> z_lengths;  
  std::vector<Index> z_strides;
  std::vector<Index> lse_lengths;
  // std::vector<Index> lse_strides;
};

// Forward Batched Arguments
struct FlashFwdBatchedParams : public BatchedParams {
  explicit FlashFwdBatchedParams(const Index b,
                                 const Index max_seqlen_q,
                                 const Index max_seqlen_kv,
                                 const Index h_q,
                                 const Index h_kv,
                                 const Index d,
                                 const torch::Tensor &q,
                                 const torch::Tensor &k,
                                 const torch::Tensor &v,
                                 torch::Tensor &out,
                                 void* z_d,
                                 void* softmax_lse_d,
                                 float p_dropout,
                                 float softmax_scale,
                                 bool is_causal)
    : BatchedParams(b,
                    max_seqlen_q,
                    max_seqlen_kv,
                    h_q,
                    h_kv,
                    d,
                    q,
                    k,
                    v,
                    out,
                    z_d,
                    softmax_lse_d,
                    p_dropout,
                    softmax_scale,
                    is_causal,
                    false) {}
};

// Backward Batched Arguments
struct FlashBwdBatchedParams : public BatchedParams {
  explicit FlashBwdBatchedParams(const Index b,
                                 const Index max_seqlen_q,
                                 const Index max_seqlen_kv,
                                 const Index h_q,
                                 const Index h_kv,
                                 const Index d,
                                 const torch::Tensor &q,
                                 const torch::Tensor &k,
                                 const torch::Tensor &v,
                                 torch::Tensor out,
                                 const torch::Tensor &dout,
                                 torch::Tensor &dq,
                                 torch::Tensor &dk,
                                 torch::Tensor &dv,
                                 void* z_d,
                                 void* softmax_lse_d,
                                 float p_dropout,
                                 float softmax_scale,
                                 bool is_causal)
    : BatchedParams(b,
                    max_seqlen_q,
                    max_seqlen_kv,
                    h_q,
                    h_kv,
                    d,
                    q,
                    k,
                    v,
                    out,
                    z_d,
                    softmax_lse_d,
                    p_dropout,
                    softmax_scale,
                    is_causal,
                    true),
      dq_ptr(dq.data_ptr()),
      dk_ptr(dk.data_ptr()),
      dv_ptr(dv.data_ptr()),
      dout_ptr(dout.data_ptr()),
      d_ptr(torch::empty({b, static_cast<long>(h_q), max_seqlen_q}, 
            q.options().dtype(torch::kFloat32)).data_ptr()) {

    Index dkv_batch_stride = dk.stride(0);
    Index dkv_seq_stride = dk.stride(-3);
    Index dkv_head_stride = dk.stride(-2);
      
    // MQA / GQA readiness
    // KGrad layout [b, max_seqlen_kv, h_q, d]
    std::vector<Index> dk_lengths{b, h_q, max_seqlen_kv, d};
    std::vector<Index> dk_strides{dkv_batch_stride, dkv_head_stride, dkv_seq_stride, 1};

    // VGrad layout [b, max_seqlen_kv, h_q, d]
    std::vector<Index> dv_lengths{b, h_q, d, max_seqlen_kv};
    std::vector<Index> dv_strides{dkv_batch_stride, dkv_head_stride, 1, dkv_seq_stride};  
  }

  void* __restrict__ dq_ptr;
  void* __restrict__ dk_ptr;
  void* __restrict__ dv_ptr;

  void* __restrict__ dout_ptr;
  void* __restrict__ d_ptr;
};

// Common Grouped Arguments
struct GroupedParams : public BaseParams {
  explicit GroupedParams(const Index b,
                         const Index max_seqlen_q,
                         const Index max_seqlen_kv,
                         const Index h_q,
                         const Index h_kv,
                         const Index d,
                         const at::Tensor &q,
                         const at::Tensor &k,
                         const at::Tensor &v,
                         torch::Tensor &out,
                         const void* cu_seqlens_q_d,
                         const void* cu_seqlens_k_d,
                         void* z_d,
                         void* softmax_lse_d,
                         float p_dropout,
                         float softmax_scale,
                         bool is_causal,
                         const bool z_permute)
    : BaseParams(b,
                 max_seqlen_q,
                 max_seqlen_kv,
                 h_q,
                 h_kv,
                 d,
                 q,
                 k,
                 v,
                 out,
                 p_dropout,
                 softmax_scale,
                 is_causal,
                 z_permute),
      seqlens_q(get_host_seqlens(static_cast<const int*>(cu_seqlens_q_d), b)),
      seqlens_kv(get_host_seqlens(static_cast<const int*>(cu_seqlens_k_d), b)) {
    
    char* q_ptr = reinterpret_cast<char*>(q.data_ptr());
    char* k_ptr = reinterpret_cast<char*>(k.data_ptr());
    char* v_ptr = reinterpret_cast<char*>(v.data_ptr());

    char* out_ptr = reinterpret_cast<char*>(out.data_ptr());
    char* z_ptr = reinterpret_cast<char*>(z_d);
    char* softmax_lse_ptr = reinterpret_cast<char*>(softmax_lse_d);  

    // TODO: move to GPU
    for (int i = 0; i < b; ++i) {
      int curr_q_batch_stride = seqlens_q[i] * q_seq_stride;
      int curr_kv_batch_stride = seqlens_kv[i] * kv_seq_stride;
      int curr_out_batch_stride = seqlens_q[i] * out_seq_stride;

      if(!is_mnko_padding && d <= 32) {
        is_mnko_padding = ((seqlens_q[i] % 128)==0 && (seqlens_kv[i] % 128)==0 ? false : true);
      } else if(!is_mnko_padding && d <= 64) {
        if(is_dropout) {
          is_mnko_padding = ((seqlens_q[i] % 128)==0 && (seqlens_kv[i] % 128)==0 ? false : true);
        } else {
          is_mnko_padding = ((seqlens_q[i] % 128)==0 && (seqlens_kv[i] % 256)==0 ? false : true);
        }
      } else if(!is_mnko_padding && d <= 128) {
        is_mnko_padding = ((seqlens_q[i] % 128)==0 && (seqlens_kv[i] % 128)==0 ? false : true);
      }

      q_ptrs.push_back(reinterpret_cast<void*>(q_ptr));
      q_ptr += get_size_in_bytes(curr_q_batch_stride, q.dtype());

      k_ptrs.push_back(reinterpret_cast<void*>(k_ptr));
      k_ptr += get_size_in_bytes(curr_kv_batch_stride, k.dtype());

      v_ptrs.push_back(reinterpret_cast<void*>(v_ptr));     
      v_ptr += get_size_in_bytes(curr_kv_batch_stride, v.dtype());      

      out_ptrs.push_back(reinterpret_cast<void*>(out_ptr));
      out_ptr += get_size_in_bytes(curr_out_batch_stride, out.dtype());

      softmax_lse_ptrs.push_back(reinterpret_cast<void*>(softmax_lse_ptr));
      softmax_lse_ptr += get_size_in_bytes(h_q * max_seqlen_q, torch::kFloat32);

      if(z_d) {
        z_ptrs.push_back(reinterpret_cast<void*>(z_ptr + i * h_q * max_seqlen_q * max_seqlen_kv * sizeof(int)));
      }
      else{
        z_ptrs.push_back(nullptr);
      }

      // Q layout [b, max_seqlen_q, h_q, d]
      std::vector<Index> q_lengths{1, h_q, seqlens_q[i], d};
      std::vector<Index> q_strides{curr_q_batch_stride, q_head_stride, q_seq_stride, 1};
 
      // K layout [b, max_seqlen_kv, h_kv, d]
      std::vector<Index> k_lengths{1, h_kv, seqlens_kv[i], d};
      std::vector<Index> k_strides{curr_kv_batch_stride, kv_head_stride, kv_seq_stride, 1};

      // V layout [b, max_seqlen_kv, h_kv, d]
      std::vector<Index> v_lengths{1, h_kv, d, seqlens_kv[i]};
      std::vector<Index> v_strides{curr_kv_batch_stride, kv_head_stride, 1, kv_seq_stride};

      // Y layout [b, max_seqlen_q, h_q, d]
      std::vector<Index> out_lengths{1, h_q, seqlens_q[i], d};
      std::vector<Index> out_strides{curr_out_batch_stride, out_head_stride, out_seq_stride, 1};

      std::vector<Index> z_lengths{1, h_q, seqlens_q[i], seqlens_kv[i]};
      std::vector<Index> z_strides = 
          z_permute ? 
          std::vector<Index>{h_q*seqlens_q[i]*seqlens_kv[i], seqlens_kv[i], h_q*seqlens_kv[i], 1} :
          // Z layout [b, max_seqlen_q, h_q, max_seqlen_kv]
          std::vector<Index>{h_q*seqlens_q[i]*seqlens_kv[i], seqlens_q[i]*seqlens_kv[i], seqlens_kv[i], 1};
          // Z layout [b, h_q, max_seqlen_q, max_seqlen_kv]

      // LSE layout [b, h_q, max_seqlen_q]
      std::vector<Index> lse_lengths{1, h_q, seqlens_q[i]};
      std::vector<Index> lse_strides{h_q*seqlens_q[i], seqlens_q[i], 1};

      q_lengths_vec.push_back(q_lengths);
      q_strides_vec.push_back(q_strides);
      k_lengths_vec.push_back(k_lengths);
      k_strides_vec.push_back(k_strides);
      v_lengths_vec.push_back(v_lengths);
      v_strides_vec.push_back(v_strides);
      out_lengths_vec.push_back(out_lengths);
      out_strides_vec.push_back(out_strides);
      z_lengths_vec.push_back(z_lengths);
      z_strides_vec.push_back(z_strides);
      lse_lengths_vec.push_back(lse_lengths);
      lse_strides_vec.push_back(lse_strides);
    }
  }

  std::vector<const void*> q_ptrs;
  std::vector<const void*> k_ptrs;
  std::vector<const void*> v_ptrs;

  std::vector<void*> out_ptrs;
  std::vector<void*> z_ptrs;
  std::vector<void*> softmax_lse_ptrs;

  std::vector<int> seqlens_q;
  std::vector<int> seqlens_kv;

  std::vector<std::vector<Index>> q_lengths_vec;
  std::vector<std::vector<Index>> q_strides_vec;
  std::vector<std::vector<Index>> k_lengths_vec;
  std::vector<std::vector<Index>> k_strides_vec;
  std::vector<std::vector<Index>> v_lengths_vec;
  std::vector<std::vector<Index>> v_strides_vec;
  std::vector<std::vector<Index>> out_lengths_vec;
  std::vector<std::vector<Index>> out_strides_vec;
  std::vector<std::vector<Index>> z_lengths_vec;
  std::vector<std::vector<Index>> z_strides_vec;
  std::vector<std::vector<Index>> lse_lengths_vec;
  std::vector<std::vector<Index>> lse_strides_vec;
};

// Forward Grouped Arguments
struct FlashFwdGroupedParams : public GroupedParams {
  explicit FlashFwdGroupedParams(const Index b,
                                 const Index max_seqlen_q,
                                 const Index max_seqlen_kv,
                                 const Index h_q,
                                 const Index h_kv,
                                 const Index d,
                                 const torch::Tensor &q,
                                 const torch::Tensor &k,
                                 const torch::Tensor &v,
                                 torch::Tensor &out,
                                 const void* cu_seqlens_q_d,
                                 const void* cu_seqlens_k_d,
                                 void* z_d,
                                 void* softmax_lse_d,
                                 float p_dropout,
                                 float softmax_scale,
                                 bool is_causal) 
    : GroupedParams(b,
                    max_seqlen_q,
                    max_seqlen_kv,
                    h_q,
                    h_kv,
                    d,
                    q,
                    k,
                    v,
                    out,
                    cu_seqlens_q_d,
                    cu_seqlens_k_d,
                    z_d,
                    softmax_lse_d,
                    p_dropout,
                    softmax_scale,
                    is_causal,
                    false) {}
};

// Backward Grouped Arguments
struct FlashBwdGroupedParams : public GroupedParams {
  explicit FlashBwdGroupedParams(const Index b,
                                 const Index max_seqlen_q,
                                 const Index max_seqlen_kv,
                                 const Index h_q,
                                 const Index h_kv,
                                 const Index d,
                                 const torch::Tensor &q,
                                 const torch::Tensor &k,
                                 const torch::Tensor &v,
                                 torch::Tensor out,
                                 const torch::Tensor &dout,
                                 torch::Tensor &dq,
                                 torch::Tensor &dk,
                                 torch::Tensor &dv,
                                 const void* cu_seqlens_q_d,
                                 const void* cu_seqlens_k_d,
                                 void* z_d,
                                 void* softmax_lse_d,
                                 float p_dropout,
                                 float softmax_scale,
                                 bool is_causal)
    : GroupedParams(b,
                    max_seqlen_q,
                    max_seqlen_kv,
                    h_q,
                    h_kv,
                    d,
                    q,
                    k,
                    v,
                    out,
                    cu_seqlens_q_d,
                    cu_seqlens_k_d,
                    z_d,
                    softmax_lse_d,
                    p_dropout,
                    softmax_scale,
                    is_causal,
                    true),
      bwd_out_ptrs(std::vector<const void*>(out_ptrs.begin(), out_ptrs.end())),
      bwd_softmax_lse_ptrs(std::vector<const void*>(softmax_lse_ptrs.begin(), softmax_lse_ptrs.end())) {
    
    char* q_ptr = reinterpret_cast<char*>(q.data_ptr());  
    char* k_ptr = reinterpret_cast<char*>(k.data_ptr());
    char* v_ptr = reinterpret_cast<char*>(v.data_ptr());

    char* dq_ptr = reinterpret_cast<char*>(dq.data_ptr());
    char* dk_ptr = reinterpret_cast<char*>(dk.data_ptr());
    char* dv_ptr = reinterpret_cast<char*>(dv.data_ptr());
    char* dout_ptr = reinterpret_cast<char*>(dout.data_ptr());

    Index dq_seq_stride = dq.stride(-3);
    Index dkv_seq_stride = dk.stride(-3);
    Index dout_seq_stride = dout.stride(-3);
    Index dq_head_stride = dq.stride(-2);
    Index dkv_head_stride = dk.stride(-2);
    Index dout_head_stride = dout.stride(-2);

    for (int i = 0; i < b; ++i) {
      // TODO: reuse it in the foward on GPU
      int curr_dq_batch_stride = seqlens_q[i] * dq_seq_stride;
      int curr_dkv_batch_stride = seqlens_kv[i] * dkv_seq_stride;
      int curr_dout_batch_stride = seqlens_q[i] * dout_seq_stride;

      if(!is_mnko_padding && d <= 32) {
        is_mnko_padding = ((seqlens_q[i] % 128)==0 && (seqlens_kv[i] % 128)==0 ? false : true);
      }
      else if(!is_mnko_padding && d <= 64) {
        is_mnko_padding = ((seqlens_q[i] % 128)==0 && (seqlens_kv[i] % 128)==0 ? false : true);
      }
      else if(!is_mnko_padding && d <= 128) {
        is_mnko_padding = ((seqlens_q[i] % 64)==0 && (seqlens_kv[i] % 128)==0 ? false : true);
      }

      dq_ptrs.push_back(reinterpret_cast<void*>(dq_ptr));
      dq_ptr += get_size_in_bytes(curr_dq_batch_stride, dq.dtype());

      dk_ptrs.push_back(reinterpret_cast<void*>(dk_ptr));
      dk_ptr += get_size_in_bytes(curr_dkv_batch_stride, dk.dtype());

      dv_ptrs.push_back(reinterpret_cast<void*>(dv_ptr));
      dv_ptr += get_size_in_bytes(curr_dkv_batch_stride, dv.dtype());

      dout_ptrs.push_back(reinterpret_cast<const void*>(dout_ptr));
      dout_ptr += get_size_in_bytes(curr_dout_batch_stride, dout.dtype());

      auto opts = q.options();
      torch::Tensor d_tensor;
      d_tensor = torch::empty({1, static_cast<long>(h_q), seqlens_q[i]}, opts.dtype(torch::kFloat32));
      d_ptrs.push_back(reinterpret_cast<void*>(d_tensor.data_ptr()));

      // MQA / GQA readiness
      // KGrad layout [b, max_seqlen_kv, h_q, d]
      std::vector<Index> dk_lengths{1, h_q, max_seqlen_kv, d};
      std::vector<Index> dk_strides{curr_dkv_batch_stride, dkv_head_stride, dkv_seq_stride, 1};

      // VGrad layout [b, max_seqlen_kv, h_q, d]
      std::vector<Index> dv_lengths{1, h_q, d, max_seqlen_kv};
      std::vector<Index> dv_strides{curr_dkv_batch_stride, dkv_head_stride, 1, dkv_seq_stride};  

      dk_lengths_vec.push_back(dk_lengths);
      dk_strides_vec.push_back(dk_strides);
      dv_lengths_vec.push_back(dv_lengths);
      dv_strides_vec.push_back(dv_strides);
    }            
  }

  std::vector<void*> dq_ptrs;
  std::vector<void*> dk_ptrs;
  std::vector<void*> dv_ptrs;

  std::vector<const void*> bwd_out_ptrs;
  std::vector<const void*> bwd_softmax_lse_ptrs;

  std::vector<const void*> dout_ptrs;
  std::vector<void*> d_ptrs;

  // MQA / GQA readiness
  std::vector<std::vector<Index>> dk_lengths_vec;
  std::vector<std::vector<Index>> dk_strides_vec;
  std::vector<std::vector<Index>> dv_lengths_vec;
  std::vector<std::vector<Index>> dv_strides_vec;
};