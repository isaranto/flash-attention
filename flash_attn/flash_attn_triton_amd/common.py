
import torch
import triton
import triton.language as tl
from triton import cdiv


def input_helper(Z, HQ, HK, N_CTX_Q, N_CTX_K, D_HEAD, dtype, layout):
    torch.manual_seed(20)

    # Initialize q, k, v
    if layout == 'bhsd':
        q_tensor_shape = (Z, HQ, N_CTX_Q, D_HEAD)
        k_tensor_shape = (Z, HK, N_CTX_K, D_HEAD)
    elif layout == 'bshd':
        q_tensor_shape = (Z, N_CTX_Q, HQ, D_HEAD)
        k_tensor_shape = (Z, N_CTX_K, HK, D_HEAD)
    else:
        assert False, 'Got unsupported tensor layout'
    q = torch.randn(q_tensor_shape, dtype=dtype, device="cuda", requires_grad=True)
    k = torch.randn(k_tensor_shape, dtype=dtype, device="cuda", requires_grad=True)
    v = torch.randn(k_tensor_shape, dtype=dtype, device="cuda", requires_grad=True)
    sm_scale = D_HEAD**-0.5
    input_metadata = MetaData(sm_scale=sm_scale)
    input_metadata.max_seqlens_q = N_CTX_Q
    input_metadata.max_seqlens_k = N_CTX_K
    input_metadata.layout = layout
    return q, k, v, input_metadata


def varlen_input_helper(Z, HQ, HK, N_CTX_Q, N_CTX_K, D_HEAD, dtype, equal_seqlens=False):
    torch.manual_seed(20)

    # Random sequence lengths. Using N_CTX as kind of max of sum of individual seqs
    if not equal_seqlens:
        max_seqlens_q = N_CTX_Q // Z
        max_seqlens_k = N_CTX_K // Z
        seqlens_q = torch.randint(1, max_seqlens_q + 1, (Z, ), dtype=torch.int32)
        seqlens_k = torch.randint(1, max_seqlens_k + 1, (Z, ), dtype=torch.int32)
    else:
        seqlens_q = torch.full((Z, ), N_CTX_Q // Z)
        seqlens_k = torch.full((Z, ), N_CTX_K // Z)

    # Calculate cumulative sequence lengths
    cu_seqlens_q = torch.cat([torch.tensor([0], dtype=torch.int32), seqlens_q.cumsum(dim=0, dtype=torch.int32)])
    cu_seqlens_k = torch.cat([torch.tensor([0], dtype=torch.int32), seqlens_k.cumsum(dim=0, dtype=torch.int32)])
    cu_seqlens_q = cu_seqlens_q.to(device="cuda")
    cu_seqlens_k = cu_seqlens_k.to(device="cuda")

    # Initialize q, k, v with variable lengths
    total_q = cu_seqlens_q[-1].item()
    total_k = cu_seqlens_k[-1].item()
    q = torch.randn((total_q, HQ, D_HEAD), dtype=dtype, device="cuda").normal_(mean=0., std=0.5).requires_grad_()
    k = torch.randn((total_k, HK, D_HEAD), dtype=dtype, device="cuda").normal_(mean=0., std=0.5).requires_grad_()
    v = torch.randn((total_k, HK, D_HEAD), dtype=dtype, device="cuda").normal_(mean=0., std=0.5).requires_grad_()
    sm_scale = D_HEAD**-0.5
    input_metadata = MetaData(sm_scale=sm_scale)
    input_metadata.set_varlen_params(cu_seqlens_q, cu_seqlens_k)
    return q, k, v, input_metadata


def get_shape_from_layout(q, k, metadata):
    if metadata.layout == 'thd':
        nheads_q, nheads_k = q.shape[1], k.shape[1]
        head_size = q.shape[-1]
        batch = metadata.num_contexts
    elif metadata.layout == 'bhsd':
        batch, nheads_q, _, head_size = q.shape
        nheads_k = k.shape[1]
    elif metadata.layout == 'bshd':
        batch, _, nheads_q, head_size = q.shape
        nheads_k = k.shape[2]
    else:
        assert False, "Got unsupported layout."
    return batch, nheads_q, nheads_k, head_size



def get_input_shapes():
    cases = [(max(1, 2**(16 - i)), 1, 2**i, 16, 1, 128)
             for i in range(8, 18)] + [(max(1, 2**(16 - i)), 1, 2**i, 16, 2, 128) for i in range(8, 18)]

    return cases

class MetaData():
    cu_seqlens_q = None
    cu_seqlens_k = None
    max_seqlens_q = 0
    max_seqlens_k = 0
    bias = None
    alibi_slopes = None
    causal = False
    num_contexts = 0
    varlen = False
    layout = None
    cache_seqlens = None
    cache_batch_idx = None
    new_kv = False
    seqlen_new = None
    k_new = None
    v_new = None
    dropout_p, return_scores= 0.0, False
    # NOTE: scale sm_scale by log_2(e) and use 2^x in the loop as we do not have native e^x support in HW.
    use_exp2 = True
    bwd_preprocessing_use_o = True
    

    def __repr__(self) -> str:
        return (f"MetaData(\n"
                f"  sm_scale={self.sm_scale},\n"
                f"  cu_seqlens_q={self.cu_seqlens_q},\n"
                f"  cu_seqlens_k={self.cu_seqlens_k},\n"
                f"  max_seqlens_q={self.max_seqlens_q},\n"
                f"  max_seqlens_k={self.max_seqlens_k},\n"
                f"  bias={self.bias},\n"
                f"  alibi_slopes={self.alibi_slopes},\n"
                f"  causal={self.causal},\n"
                f"  num_contexts={self.num_contexts},\n"
                f"  varlen={self.varlen},\n"
                f"  layout={self.layout},\n"
                f"  cache_seqlens={self.cache_seqlens},\n"
                f"  cache_batch_idx={self.cache_batch_idx},\n"
                f"  new_kv={self.new_kv},\n"
                f"  seqlen_new={self.seqlen_new},\n"
                f"  k_new={self.k_new},\n"
                f"  v_new={self.v_new},\n"
                f"  dropout_p={self.dropout_p},\n"
                f"  return_scores={self.return_scores}\n"
                f")")

    def __init__(self, sm_scale=1.0):
        self.sm_scale = sm_scale

    def set_varlen_params(self, cu_seqlens_q, cu_seqlens_k):
        self.varlen = True
        self.layout = 'thd'
        self.cu_seqlens_q = cu_seqlens_q
        self.cu_seqlens_k = cu_seqlens_k
        # Without "varlen", there should still be one sequence.
        assert len(cu_seqlens_q) >= 2
        assert len(cu_seqlens_q) == len(cu_seqlens_k)
        self.num_contexts = len(cu_seqlens_q) - 1
        for i in range(0, self.num_contexts):
            self.max_seqlens_q = max(cu_seqlens_q[i + 1].item() - cu_seqlens_q[i].item(), self.max_seqlens_q)
            self.max_seqlens_k = max(cu_seqlens_k[i + 1].item() - cu_seqlens_k[i].item(), self.max_seqlens_k)

    def need_bias(self, bias, batch, nheads, seqlen_q, seqlen_k):
        assert bias.is_cuda
        assert bias.dim() == 4
        assert bias.shape[0] == 1
        assert bias.shape[2:] == (seqlen_q, seqlen_k)
        self.bias = bias

    def need_alibi(self, alibi_slopes, batch, nheads):
        assert alibi_slopes.is_cuda
        assert alibi_slopes.dim() == 2
        assert alibi_slopes.shape[0] == batch
        assert alibi_slopes.shape[1] == nheads
        self.alibi_slopes = alibi_slopes

    def need_causal(self):
        self.causal = True

    def need_dropout(self, dropout_p, return_scores):
        self.dropout_p = dropout_p
        self.return_scores = return_scores

    def check_args(self, q, k, v, o):
        assert q.dim() == k.dim() and q.dim() == v.dim()

        batch, nheads_q, nheads_k, head_size = get_shape_from_layout(q, k, self)
        if self.varlen:
            assert q.dim() == 3
            assert self.cu_seqlens_q is not None
            assert self.cu_seqlens_k is not None
            assert len(self.cu_seqlens_q) == len(self.cu_seqlens_k)
            # TODO: Remove once bias is supported with varlen
            assert self.bias is None
            # TODO:Remove once dropout is supported with varlen
            assert self.dropout_p == 0.0
            # assert not self.return_scores
        else:
            assert q.dim() == 4
            assert self.max_seqlens_q > 0 and self.max_seqlens_k > 0
            assert self.cu_seqlens_q is None and self.cu_seqlens_k is None
        assert k.shape == v.shape
        assert q.shape[-1] == k.shape[-1] and q.shape[-1] == v.shape[-1]
        # TODO: Change assert if we support qkl f8 and v f16
        assert q.dtype == k.dtype and q.dtype == v.dtype
        assert head_size <= 256
        assert o.shape == q.shape
        assert (nheads_q % nheads_k) == 0
        assert self.layout is not None
        assert self.layout == 'thd' or not self.varlen





