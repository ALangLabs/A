#include "type-checker/sema_internal.hpp"

namespace a {

TypePtr Sema::checkBuiltinKernels(const std::string& name, CallExpr& call) {

  if (name.rfind("kernels_", 0) == 0) {
    inferCallArgTypes(call);
    const std::string suffix = name.substr(8);
    auto numericElem = [&]() {
      return freshVar(Constraint::Numeric, call.span,
                      "the numeric element type of '" + name + "'");
    };
    if (suffix == "reduce_argmax" || suffix == "reduce_argmin") {
      return typeInt(64, true);
    }
    if (suffix == "reduce_mean" || suffix == "reduce_var" || suffix == "reduce_std" ||
        suffix == "reduce_norm") {
      return typeFloat(64);
    }
    if (suffix == "dot" || suffix == "reduce_sum" || suffix == "reduce_max" ||
        suffix == "reduce_min" || suffix == "reduce_prod" || suffix == "trace") {
      return numericElem();
    }
    if (suffix == "softmax" || suffix == "sigmoid" || suffix == "tanh" ||
        suffix == "normalize" || suffix == "bias_relu" || suffix == "gelu" ||
        suffix == "silu" || suffix == "elu" || suffix == "leaky_relu" ||
        suffix == "softplus" || suffix == "mish" || suffix == "rms_norm" ||
        suffix == "layer_norm" || suffix == "attention_scores" || suffix == "attention" ||
        suffix == "avg_pool1d" || suffix == "dequantize_int8") {
      return typeArray(typeFloat(64));
    }
    if (suffix == "softmax_rows" || suffix == "layer_norm_rows") {
      return typeArray(typeArray(typeFloat(64)));
    }
    if (suffix == "matmul" || suffix == "transpose" || suffix == "outer" ||
        suffix == "eye") {
      return typeArray(typeArray(numericElem()));
    }
    if (suffix == "zeros" || suffix == "ones" || suffix == "full" || suffix == "reshape") {
      return typeArray(typeError());
    }
    if (suffix == "quantize_int8") return typeArray(typeInt(64, true));
    return typeArray(numericElem());
  }

  return nullptr;
}

}  // namespace a
