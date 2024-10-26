import argparse
import torch
import triton
from flash_attn.flash_attn_triton_amd.utils import (
    MetaData,
    input_helper,
    varlen_input_helper,
)
from flash_attn.flash_attn_triton_amd.interface_torch import attention_prefill, attention_decode


def get_benchmark_configs(args, varlen=False):
    """
    Returns benchmark configurations based on whether variable-length sequences are used.
    """
    if args.custom_config:
        hk = args.hq if not args.hk else args.hk
        sk = args.sq if not args.sk else args.sk
        return [(args.b, args.hq, hk, args.sq, sk)]
    elif varlen:
        return [
            (2, 16, 4, 1024, 1024),
            (8, 16, 2, 2048, 2048),
            (4, 16, 8, 4096, 4096),
            (2, 16, 4, 8192, 8192),
            (2, 16, 8, 16384, 16384),
            (2, 48, 12, 1024, 1024),
            (2, 48, 24, 2048, 2048),
            (2, 48, 8, 4096, 4096),
            (2, 48, 4, 8192, 8192),
            (2, 48, 2, 16384, 16384),
            (2, 64, 32, 1024, 1024),
            (4, 64, 16, 2048, 2048),
            (4, 64, 8, 4096, 4096),
            (4, 64, 32, 8192, 8192),
            (4, 128, 16, 16384, 16384),
        ]
    else:
        return [
            (16, 16, 16, 1024, 1024),
            (8, 16, 16, 2048, 2048),
            (4, 16, 16, 4096, 4096),
            (2, 16, 16, 8192, 8192),
            (1, 16, 16, 16384, 16384),
            (2, 48, 48, 1024, 1024),
            (2, 48, 48, 2048, 1024),
            (2, 48, 48, 4096, 8192),
            (2, 48, 48, 8192, 4096),
            (2, 48, 48, 16384, 8192),
            (8, 16, 16, 1989, 15344),
            (4, 16, 16, 4097, 163),
            (2, 16, 16, 8122, 2159),
            (1, 16, 16, 16281, 7),
            (2, 48, 48, 1021, 1020),
            (2, 48, 48, 2001, 2048),
            (2, 48, 48, 3996, 9639),
            (2, 48, 48, 8181, 1021),
        ]


def gen_fn_inputs(fn_name, BATCH, HQ, HK, N_CTX_Q, N_CTX_K, D_HEAD, dtype, device, layout, causal):
    is_varlen = layout == "thd"

    if fn_name.startswith("prefill"):
        if is_varlen:
            q, k, v, input_metadata = varlen_input_helper(
                BATCH, HQ, HK, N_CTX_Q, N_CTX_K, D_HEAD, dtype, device=device)
            for i in range(input_metadata.num_contexts):
                seqlen_q = input_metadata.cu_seqlens_q[i + 1] - input_metadata.cu_seqlens_q[i]
                seqlen_k = input_metadata.cu_seqlens_k[i + 1] - input_metadata.cu_seqlens_k[i]
                flops_per_matmul += seqlen_q.item() * seqlen_k.item() * HQ * D_HEAD * 2
        else:
            q, k, v, input_metadata = input_helper(
                BATCH, HQ, HK, N_CTX_Q, N_CTX_K, D_HEAD, dtype, layout, device=device
            )
            flops_per_matmul = 2.0 * BATCH * HQ * N_CTX_Q * N_CTX_K * D_HEAD

        if causal:
            input_metadata.need_causal()

        o = torch.empty_like(q)
        input_data = (q, k, v, o, input_metadata)
    elif fn_name.startswith("decode"):
        q = torch.randn(
            [BATCH, N_CTX_Q, HK, HQ // HK, D_HEAD],
            device=device,
            dtype=dtype,
            requires_grad=False,
        )
        k = torch.randn(
            [BATCH, N_CTX_K, HK, 1, D_HEAD],
            device=device,
            dtype=dtype,
            requires_grad=False,
        ).expand(-1, -1, -1, HQ // HK, -1)
        v = torch.randn(
            [BATCH, N_CTX_K, HK, 1, D_HEAD],
            device=device,
            dtype=dtype,
            requires_grad=False,
        ).expand(-1, -1, -1, HQ // HK, -1)
        input_metadata = MetaData(sm_scale=1.3)
        input_metadata.layout = "bsghd"
        
        # Adjust flops calculation if needed
        input_data = (q, k, v, input_metadata)
        flops_per_matmul = 2.0 * BATCH * HQ * N_CTX_Q * N_CTX_K * D_HEAD
    else:
        raise ValueError("Unsupported benchmark function")

    
    return input_data, flops_per_matmul

def run_benchmark(args, fn_name, fn):
    """
    Runs the benchmark for the provided function based on the provided arguments.
    """
    print(f"Benching {fn_name}...")

    dtype = ARGS_TO_TORCH_DTYPE[args.dtype]
    head_size = args.d if args.d else 128
    causal = args.causal
    varlen = args.layout == "thd"
    print_time = args.return_time
    line_names = "Time (ms)" if print_time else "TFLOPS"

    # Determine configurations
    x_vals_list = get_benchmark_configs(args, varlen=varlen)

    # Setup benchmark configurations
    configs = [
        triton.testing.Benchmark(
            x_names=["BATCH", "HQ", "HK", "N_CTX_Q", "N_CTX_K"],
            x_vals=x_vals_list,
            line_arg="provider",
            line_vals=["triton"],
            line_names=[line_names],
            styles=[("red", "-")],
            ylabel="ms",
            plot_name=f"benchmark-{fn_name}-d{head_size}-layout{args.layout}",
            args={
                "D_HEAD": head_size,
                "dtype": dtype,
                "causal": causal,
            },
        )
    ]

    @triton.testing.perf_report(configs)
    def bench_function(
        BATCH, HQ, HK, N_CTX_Q, N_CTX_K, D_HEAD, dtype, causal, provider, device="cuda"
    ):
        warmup = 25
        rep = 100
        flops_per_matmul = 0

        fn_inputs, flops_per_matmul = gen_fn_inputs(fn_name, BATCH, HQ, HK, N_CTX_Q, N_CTX_K, D_HEAD, dtype, device, args.layout, causal)
        ms = triton.testing.do_bench(lambda: fn(*fn_inputs), warmup=warmup, rep=rep)

        # NOTE: taken from bench old. Not sure if true 2.0(fwd) + 2.0(bwd) + 0.5(recompute)
        total_flops = 2 * flops_per_matmul
        if causal:
            total_flops *= 0.5

        if print_time:
            return ms
        else:
            return total_flops / ms * 1e-9

    bench_function.run(save_path=".", print_data=True)


def supported_layouts():
    """
    Returns a string describing the supported layouts.
    """
    return (
        "bhsd: Q, K, V are individual tensors of [batch, num_heads, seqlen_q/k, head_size]\n"
        "bshd: Q, K, V are individual tensors of [batch, seqlen_q/k, num_heads, head_size]\n"
        "thd: Q, K, V are individual tensors of [total_q/k, num_heads, head_size]\n"
        'This layout is sometimes called "varlen" or "grouped" layout.'
    )


def parse_args():
    """
    Parses command-line arguments.
    """
    parser = argparse.ArgumentParser(
        prog="Benchmark FlashAttention",
        allow_abbrev=False,
    )
    parser.add_argument("-b", type=int, default=0)
    parser.add_argument("-hq", type=int, default=0)
    parser.add_argument("-hk", type=int, default=0)
    parser.add_argument("-sq", type=int, default=0)
    parser.add_argument("-sk", type=int, default=0)
    parser.add_argument(
        "-equal_seqlens",
        action="store_true",
        default=False,
        help="If specified, each context within the thd layout has same seqlen as sq and sk",
    )
    parser.add_argument("-d", type=int, default=0)
    parser.add_argument("-causal", action="store_true", default=False)
    parser.add_argument("-dtype", default="fp16")
    parser.add_argument("-return_time", action="store_true", default=True)
    parser.add_argument(
        "-layout",
        type=str,
        default="bhsd",
        help=supported_layouts(),
    )
    parser.add_argument(
        "-benchmark_fn",
        type=str,
        nargs="*",
        choices=FUNCTIONS.keys(),
        help="Function(s) to benchmark: prefill, decode, or both",
    )
    return parser.parse_args()

ARGS_TO_TORCH_DTYPE = {
    "fp16": torch.float16,
    "bf16": torch.bfloat16,
    "fp32": torch.float32,
}

FUNCTIONS = {
    "prefill": attention_prefill,
    "decode": attention_decode
}

def main():
    """
    Main function to run benchmarks.
    """
    args = parse_args()

    # Validate arguments
    assert (
        args.layout == "thd" or not args.equal_seqlens
    ), "Equal sequence lengths arg must be used with the thd layout."
    args.custom_config = False
    if args.b or args.hq or args.hk or args.sq or args.sk or args.d:
        args.custom_config = True
        assert args.b and args.hq and args.sq and args.d, (
            "If custom config is specified, please provide all of batch, "
            "number of Q heads, Q sequence length, and head size."
        )
    assert args.dtype in ARGS_TO_TORCH_DTYPE, "Only fp16, bf16 and fp32 types currently supported."

    # determine the functions to benchmark
    if args.benchmark_fn is None or len(args.benchmark_fn) == 0:
        bench_fn_list = FUNCTIONS.keys()
    else:
        bench_fn_list = args.benchmark_fn
    
    # bench functions
    for fn_name in bench_fn_list:
        if fn_name not in FUNCTIONS:
            raise ValueError(f"Invalid benchmark function specified: {fn_name}")
        run_benchmark(args, fn_name, FUNCTIONS[fn_name])


if __name__ == "__main__":
    main()
