#pragma once

#include "../../cudnn_frontend_Heuristics.h"
#include "../../cudnn_frontend_Logging.h"

#include "../graph_helpers.h"
#include "../node_interface.h"

#include "matmul.h"
#include "pointwise.h"
#include "rng.h"
#include "softmax.h"

namespace cudnn_frontend::graph {

class ScaledDotProductFlashAttentionNode : public INode {
    using input_names  = Scaled_dot_product_flash_attention_attributes::input_names;
    using output_names = Scaled_dot_product_flash_attention_attributes::output_names;

    std::shared_ptr<Tensor_attributes> rng_output;
    std::shared_ptr<Tensor_attributes> dropout_scale;
    std::shared_ptr<Tensor_attributes> negative_inf_causal;
    std::shared_ptr<Tensor_attributes> negative_inf_padding;
    std::shared_ptr<Tensor_attributes> alibi_slopes;

   public:
    Scaled_dot_product_flash_attention_attributes attributes;

    ScaledDotProductFlashAttentionNode(Scaled_dot_product_flash_attention_attributes&& attributes_,
                                       detail::Context const& context)
        : INode(context), attributes(std::move(attributes_)) {}

    Type
    getType() override final {
        return Type::COMPOSITE;
    }

    error_t
    pre_validate_node() const override final {
        getLogger() << "[cudnn_frontend] INFO: "
                    << "Validating ScaledDotProductFlashAttentionNode " << attributes.name << "..." << std::endl;

        CUDNN_FE_VALIDATE_INPUT_TENSOR(input_names::Q);
        CUDNN_FE_VALIDATE_INPUT_TENSOR(input_names::K);
        CUDNN_FE_VALIDATE_INPUT_TENSOR(input_names::V);

        CUDNN_FE_VALIDATE_OUTPUT_TENSOR(output_names::O);

#define CUDNN_FE_VALIDATE_STRIDE(port, port_map)                                                                \
    {                                                                                                           \
        auto const& t = port_map.find(port);                                                                    \
        RETURN_CUDNN_FRONTEND_ERROR_IF(                                                                         \
            t->second->get_stride().back() != 1,                                                                \
            error_code_t::GRAPH_NOT_SUPPORTED,                                                                  \
            "The stride for the last dimension corresponding to the embedding size per head should be 1 for " + \
                std::string(#port));                                                                            \
    }

        CUDNN_FE_VALIDATE_STRIDE(input_names::Q, attributes.inputs);
        CUDNN_FE_VALIDATE_STRIDE(input_names::K, attributes.inputs);
        CUDNN_FE_VALIDATE_STRIDE(input_names::V, attributes.inputs);

#undef CUDNN_FE_VALIDATE_STRIDE

        RETURN_CUDNN_FRONTEND_ERROR_IF(attributes.is_inference.has_value() == false,
                                       error_code_t::ATTRIBUTE_NOT_SET,
                                       "is_infernece attribute not set");

        auto const& dropout_mask    = attributes.inputs.find(input_names::Dropout_mask);
        bool const has_dropout_mask = (dropout_mask != attributes.inputs.end()) && (dropout_mask->second != nullptr);
        RETURN_CUDNN_FRONTEND_ERROR_IF(attributes.dropout_probability.has_value() && has_dropout_mask,
                                       error_code_t::ATTRIBUTE_NOT_SET,
                                       "Using both, custom dropout mask and internal-mask generation using dropout "
                                       "probability, is ill-formed.");

        RETURN_CUDNN_FRONTEND_ERROR_IF(
            attributes.dropout_probability.has_value() && attributes.dropout_probability.value() == 1.0,
            error_code_t::ATTRIBUTE_NOT_SET,
            "Dropout probability cannot be 1 as corresponding scale wont be well formed.");

        RETURN_CUDNN_FRONTEND_ERROR_IF(context.get_intermediate_data_type() == DataType_t::NOT_SET,
                                       error_code_t::ATTRIBUTE_NOT_SET,
                                       "Intermediate tensor data type needs to be set as internal tensors require it.");

        auto const& seq_len_q    = attributes.inputs.find(input_names::SEQ_LEN_Q);
        bool const has_seq_len_q = (seq_len_q != attributes.inputs.end()) && (seq_len_q->second != nullptr);

        auto const& seq_len_kv    = attributes.inputs.find(input_names::SEQ_LEN_KV);
        bool const has_seq_len_kv = (seq_len_kv != attributes.inputs.end()) && (seq_len_kv->second != nullptr);
        RETURN_CUDNN_FRONTEND_ERROR_IF(attributes.padding_mask && (!has_seq_len_q || !has_seq_len_kv),
                                       error_code_t::ATTRIBUTE_NOT_SET,
                                       "Padding mask requires seq_len_q and seq_len_kv to be set.");

        RETURN_CUDNN_FRONTEND_ERROR_IF((!attributes.padding_mask) && (has_seq_len_q || has_seq_len_kv),
                                       error_code_t::ATTRIBUTE_NOT_SET,
                                       "seq_len_q and seq_len_kv needs to be set only if padding mask is enabled.");

        auto const& attn_scale    = attributes.inputs.find(input_names::Attn_scale);
        bool const has_attn_scale = (attn_scale != attributes.inputs.end()) && (attn_scale->second != nullptr);
        RETURN_CUDNN_FRONTEND_ERROR_IF(has_attn_scale && attributes.attn_scale_value.has_value(),
                                       error_code_t::ATTRIBUTE_NOT_SET,
                                       "attn_scale with tensor and value cannot be set at the same time.");

        auto it         = attributes.inputs.find(input_names::Q);
        auto q_dim      = it->second->get_dim();
        auto hidden_dim = q_dim[3];

        RETURN_CUDNN_FRONTEND_ERROR_IF((((hidden_dim <= 128) && (hidden_dim % 8 == 0)) == false),
                                       error_code_t::GRAPH_NOT_SUPPORTED,
                                       "Num hidden_dim shoud be less than 128 and hidden_dim should be multiple of 8");

        auto attn_mask = attributes.inputs.find(input_names::Bias);
        if (attn_mask != attributes.inputs.end() && attn_mask->second != nullptr) {
            auto attn_mask_dtype = attn_mask->second->get_data_type();
            RETURN_CUDNN_FRONTEND_ERROR_IF((attn_mask_dtype == DataType_t::BOOLEAN),
                                           error_code_t::GRAPH_NOT_SUPPORTED,
                                           "Attn mask data type cannot be boolean");
        }

        CHECK_CUDNN_FRONTEND_ERROR(attributes.validate_inputs());
        return {error_code_t::OK, ""};
    }

    error_t
    expand_and_infer_properties() override final {
        getLogger() << "[cudnn_frontend] INFO: Inferrencing properties for Scaled_dot_product_flash_attention node  "
                    << attributes.name << "..." << std::endl;

        // DO NOT REMOVE
        // input data type is needed for:
        // - aType of bmm2
        // - dropout scale in pre 8.9.3
        attributes.fill_from_context(context);

        // Gather dims to fill properties of virtual tensors
        auto const& q_dim = attributes.inputs[input_names::Q]->get_dim();
        auto b            = q_dim[0];
        auto h            = q_dim[1];
        auto s_q          = q_dim[2];
        auto const& k_dim = attributes.inputs[input_names::K]->get_dim();
        auto s_kv         = k_dim[2];
        auto const& v_dim = attributes.inputs[input_names::V]->get_dim();
        auto d_v          = v_dim[3];

        // cuDNN frontend API attention requires Q, K, V where
        // Q = {b, h, s_q, d_qk}
        // K = {b, h, s_kv, d_qk}
        // V = {b, h, s_kv, d_v}
        // but cuDNN backend API attention requires Q, KT, V
        // Q = {b, h, s_q, d_qk}
        // KT = {b, h, d_qk, s_kv}
        // V = {b, h, s_kv, d_v}
        // So the code below maps the K->KT
        std::vector<int64_t> temp_vec;

        temp_vec = attributes.inputs[input_names::K]->get_dim();
        std::swap(temp_vec[2], temp_vec[3]);
        attributes.inputs[input_names::K]->set_dim(temp_vec);

        temp_vec = attributes.inputs[input_names::K]->get_stride();
        std::swap(temp_vec[2], temp_vec[3]);
        attributes.inputs[input_names::K]->set_stride(temp_vec);

        std::shared_ptr<Tensor_attributes> last_output;

        auto bmm1_attributes = Matmul_attributes()
                                   .set_name("bmm1")
                                   .set_m_override(attributes.inputs[input_names::SEQ_LEN_Q])
                                   .set_n_override(attributes.inputs[input_names::SEQ_LEN_KV]);
        auto const& bmm1_output =
            matmul(attributes.inputs[input_names::Q], attributes.inputs[input_names::K], bmm1_attributes);
        // Setting dims and strides as pointwise op wont have knowledge of how to do it for mha.
        bmm1_output->set_dim({b, h, s_q, s_kv}).set_stride({h * s_q * s_kv, s_q * s_kv, s_kv, 1});
        last_output = bmm1_output;

        // Optional scale
        if (attributes.attn_scale_value.has_value()) {
            attributes.inputs[input_names::Attn_scale] = std::make_shared<Tensor_attributes>();
            attributes.inputs[input_names::Attn_scale]
                ->set_dim({1, 1, 1, 1})
                .set_stride({1, 1, 1, 1})
                .set_data_type(DataType_t::FLOAT)
                .set_is_pass_by_value(true);
        }
        if (attributes.inputs[input_names::Attn_scale]) {
            Pointwise_attributes scale_attributes;
            scale_attributes.set_name("attn_scale").set_mode(PointwiseMode_t::MUL);
            auto const& attn_scale_output =
                pointwise(last_output, attributes.inputs[input_names::Attn_scale], scale_attributes);
            last_output = attn_scale_output;
        }

        // Optional bias
        if (attributes.inputs[input_names::Bias]) {
            auto add_attributes     = Pointwise_attributes().set_name("bias").set_mode(PointwiseMode_t::ADD);
            auto const& bias_output = pointwise(last_output, attributes.inputs[input_names::Bias], add_attributes);
            last_output             = bias_output;
        }

        if (attributes.alibi_mask) {
            auto row_index_attributes = Pointwise_attributes()
                                            .set_name("gen_row_index")
                                            .set_mode(PointwiseMode_t::GEN_INDEX)
                                            .set_axis(2)
                                            .set_compute_data_type(DataType_t::INT32);
            auto const& row_index_output = pointwise(last_output, row_index_attributes);
            row_index_output->set_data_type(DataType_t::INT32);

            auto col_index_attributes = Pointwise_attributes()
                                            .set_name("gen_col_index")
                                            .set_mode(PointwiseMode_t::GEN_INDEX)
                                            .set_axis(3)
                                            .set_compute_data_type(DataType_t::INT32);
            auto const& col_index_output = pointwise(last_output, col_index_attributes);
            col_index_output->set_data_type(DataType_t::INT32);

            auto sub_attributes = Pointwise_attributes()
                                      .set_name("sub")
                                      .set_mode(PointwiseMode_t::SUB)
                                      .set_compute_data_type(DataType_t::INT32);
            auto const& sub_output = pointwise(col_index_output, row_index_output, sub_attributes);
            sub_output->set_data_type(DataType_t::INT32);

            // Multiply by alibi slope
            alibi_slopes = std::make_shared<Tensor_attributes>();
            alibi_slopes->set_dim({1, h, 1, 1})
                .set_stride({h, 1, 1, 1})
                // Hard code data type float as FE itself will compute and place in variant pack later
                .set_data_type(DataType_t::FLOAT);

            auto mul_attributes    = Pointwise_attributes().set_name("mul").set_mode(PointwiseMode_t::MUL);
            auto const& alibi_mask = pointwise(sub_output, alibi_slopes, mul_attributes);

            // Add alibi_mask
            auto add_attributes    = Pointwise_attributes().set_name("add").set_mode(PointwiseMode_t::ADD);
            auto const& add_output = pointwise(last_output, alibi_mask, add_attributes);
            last_output            = add_output;
        }

        if (attributes.padding_mask) {
            auto row_index_attributes = Pointwise_attributes()
                                            .set_name("gen_row_index")
                                            .set_mode(PointwiseMode_t::GEN_INDEX)
                                            .set_axis(2)
                                            .set_compute_data_type(DataType_t::INT32);
            auto const& row_index_output = pointwise(last_output, row_index_attributes);
            row_index_output->set_data_type(DataType_t::INT32);

            auto col_index_attributes = Pointwise_attributes()
                                            .set_name("gen_col_index")
                                            .set_mode(PointwiseMode_t::GEN_INDEX)
                                            .set_axis(3)
                                            .set_compute_data_type(DataType_t::INT32);
            auto const& col_index_output = pointwise(last_output, col_index_attributes);
            col_index_output->set_data_type(DataType_t::INT32);

            auto row_less_seq_q_attributes = Pointwise_attributes()
                                                 .set_name("row_less_seq_q")
                                                 .set_mode(PointwiseMode_t::CMP_LT)
                                                 .set_compute_data_type(DataType_t::INT32);
            auto const& row_less_seq_q_output =
                pointwise(row_index_output, attributes.inputs[input_names::SEQ_LEN_Q], row_less_seq_q_attributes);
            row_less_seq_q_output->set_data_type(DataType_t::INT32);

            auto col_less_seq_kv_attributes = Pointwise_attributes()
                                                  .set_name("col_less_seq_kv")
                                                  .set_mode(PointwiseMode_t::CMP_LT)
                                                  .set_compute_data_type(DataType_t::INT32);
            auto const& col_less_seq_kv_output =
                pointwise(col_index_output, attributes.inputs[input_names::SEQ_LEN_KV], col_less_seq_kv_attributes);
            col_less_seq_kv_output->set_data_type(DataType_t::INT32);

            auto logical_and_attributes = Pointwise_attributes()
                                              .set_name("logical_and")
                                              .set_mode(PointwiseMode_t::LOGICAL_AND)
                                              .set_compute_data_type(DataType_t::BOOLEAN);
            auto const& logical_and_output =
                pointwise(row_less_seq_q_output, col_less_seq_kv_output, logical_and_attributes);
            logical_and_output->set_data_type(DataType_t::BOOLEAN);

            // Lower attributes to binary select attributes
            negative_inf_padding = std::make_shared<Tensor_attributes>();
            negative_inf_padding->set_dim({1, 1, 1, 1})
                .set_stride({1, 1, 1, 1})
                .set_is_pass_by_value(true)
                // Hard code data type float as FE itself will place FLOAT_MIN in variant pack later
                .set_data_type(DataType_t::FLOAT);

            auto binary_select_attributes =
                Pointwise_attributes().set_name("binary_select").set_mode(PointwiseMode_t::BINARY_SELECT);
            auto const& padding_mask_output =
                pointwise(last_output, negative_inf_padding, logical_and_output, binary_select_attributes);
            last_output = padding_mask_output;
        }

        if (attributes.causal_mask) {
            auto row_index_attributes =
                Pointwise_attributes().set_name("gen_row_index").set_mode(PointwiseMode_t::GEN_INDEX).set_axis(2);
            auto const& row_index_output = pointwise(last_output, row_index_attributes);

            auto col_index_attributes =
                Pointwise_attributes().set_name("gen_col_index").set_mode(PointwiseMode_t::GEN_INDEX).set_axis(3);
            auto const& col_index_output = pointwise(last_output, col_index_attributes);

            auto greater_than_attributes = Pointwise_attributes()
                                               .set_name("row_greater_than_col")
                                               .set_mode(PointwiseMode_t::CMP_GE)
                                               .set_compute_data_type(DataType_t::BOOLEAN);
            auto const& row_greater_than_col_output =
                pointwise(row_index_output, col_index_output, greater_than_attributes);
            row_greater_than_col_output->set_data_type(DataType_t::BOOLEAN);

            // Lower attributes to binary select attributes
            negative_inf_causal = std::make_shared<Tensor_attributes>();
            negative_inf_causal->set_dim({1, 1, 1, 1})
                .set_stride({1, 1, 1, 1})
                .set_is_pass_by_value(true)
                // Hard code data type float as FE itself will place FLOAT_MIN in variant pack later
                .set_data_type(DataType_t::FLOAT);

            auto binary_select_attributes =
                Pointwise_attributes().set_name("binary_select").set_mode(PointwiseMode_t::BINARY_SELECT);
            auto const& causal_mask_output =
                pointwise(last_output, negative_inf_causal, row_greater_than_col_output, binary_select_attributes);
            last_output = causal_mask_output;
        }

        // Lower attributes to softmax attributes
        auto softmax_output = std::make_shared<Tensor_attributes>();
        softmax_output->set_is_virtual(true);

        // Create a virtual output for stats if inference step otherwise output.Stats is already set
        auto softmax_stats = attributes.outputs[output_names::Stats];
        if (attributes.is_inference.value() == true) {
            softmax_stats = std::make_shared<Tensor_attributes>();
            softmax_stats->set_is_virtual(true);
        }

        auto softmax_attributes =
            Softmax_attributes().set_name("softmax").has_stats(true).has_M_Zinv(false);  // As this is flash attention
        // Special non-functional-style call. Needed because output already created and provided to user.
        softmax(last_output, softmax_attributes, softmax_output, softmax_stats);
        last_output = softmax_output;

        // Two cases for training: dropout present or not
        bool dropout_present = false;
        if (attributes.dropout_probability.has_value()) {
            dropout_present = true;
            // Special case: Skip dropout when 0.0 probability. Only do for 8.9.3 and up as rng isn't optional earlier.
            if (cudnnGetVersion() > 8902 && attributes.dropout_probability.value() == 0.0) {
                dropout_present = false;
            }
        } else if (attributes.inputs[input_names::Dropout_mask]) {
            dropout_present = true;
        }

        if (dropout_present) {
            if (attributes.outputs[output_names::RNG_DUMP] != nullptr) {
                rng_output = attributes.outputs[output_names::RNG_DUMP];
                rng(attributes.inputs[input_names::Seed],
                    attributes.inputs[input_names::Offset],
                    Rng_attributes()
                        .set_name("rng")
                        .set_distribution(RngDistribution_t::BERNOULLI)
                        .set_bernoulli_probability(1.0 - attributes.dropout_probability.value()),
                    rng_output);
            } else {
                rng_output = rng(attributes.inputs[input_names::Seed],
                                 attributes.inputs[input_names::Offset],
                                 Rng_attributes()
                                     .set_name("rng")
                                     .set_distribution(RngDistribution_t::BERNOULLI)
                                     .set_bernoulli_probability(1.0 - attributes.dropout_probability.value()));
                rng_output
                    // Hard coding dims and strides as rng output can no inputs to infer it from.
                    ->set_dim({b, h, s_q, s_kv})
                    .set_stride({h * s_q * s_kv, s_q * s_kv, s_kv, 1});
            }

            auto mask_attributes = Pointwise_attributes().set_name("dropout_mask_mul").set_mode(PointwiseMode_t::MUL);
            auto const& dropout_mask_output = pointwise(last_output, rng_output, mask_attributes);
            last_output                     = dropout_mask_output;

            dropout_scale = std::make_shared<Tensor_attributes>();
            dropout_scale->set_dim({1, 1, 1, 1})
                .set_stride({1, 1, 1, 1})
                .set_is_pass_by_value(true)
// Hard code data type input type as FE itself will place value in variant pack later
#if CUDNN_VERSION < 8903
                .set_data_type(attributes.inputs[input_names::Q]->get_data_type());
#else
                .set_data_type(DataType_t::FLOAT);
#endif

            auto dropout_scale_attributes =
                Pointwise_attributes().set_name("dropout_scale").set_mode(PointwiseMode_t::MUL);
            auto const& dropout_scale_output = pointwise(last_output, dropout_scale, dropout_scale_attributes);
            last_output                      = dropout_scale_output;
        }

        // Lower attributes to bmm2 attributes
        // Requirement by cudnn backend to take in bmm2 aType as i/o type.
        last_output->set_data_type(attributes.inputs[input_names::Q]->get_data_type());

        auto const& seq_len_q  = attributes.inputs[input_names::SEQ_LEN_Q];
        auto const& seq_len_kv = attributes.inputs[input_names::SEQ_LEN_KV];
        auto const& V          = attributes.inputs[input_names::V];
        auto const& O          = attributes.outputs[output_names::O];
        auto bmm2_attributes =
            Matmul_attributes().set_name("bmm2").set_m_override(seq_len_q).set_k_override(seq_len_kv);
        // Special non-functional-style call. Needed because output already created and provided to user.
        matmul(last_output, V, bmm2_attributes, O);

        // Set dims if user did not
        if (attributes.outputs[output_names::O]->get_dim().empty()) {
            attributes.outputs[output_names::O]->set_dim({b, h, s_q, d_v});
        }
        if (attributes.outputs[output_names::O]->get_stride().empty()) {
            auto const O_dim = attributes.outputs[output_names::O]->get_dim();
            attributes.outputs[output_names::O]->set_stride(
                {O_dim[3] * O_dim[2] * O_dim[1], O_dim[3] * O_dim[2], O_dim[3], 1});
        }

        return {error_code_t::OK, ""};
    }

    error_t
    post_validate_node() const override final {
#define CUDNN_FE_VALIDATE_STRIDE(port, port_map)                                                                \
    {                                                                                                           \
        auto const& t = port_map.find(port);                                                                    \
        RETURN_CUDNN_FRONTEND_ERROR_IF(                                                                         \
            t->second->get_stride().back() != 1,                                                                \
            error_code_t::GRAPH_NOT_SUPPORTED,                                                                  \
            "The stride for the last dimension corresponding to the embedding size per head should be 1 for " + \
                std::string(#port));                                                                            \
    }

        CUDNN_FE_VALIDATE_STRIDE(output_names::O, attributes.outputs);

#undef CUDNN_FE_VALIDATE_STRIDE

        // Validate outputs
        // All properties of output tensors should have been set now.
        CHECK_CUDNN_FRONTEND_ERROR(attributes.validate_outputs());

        return {error_code_t::OK, ""};
    }

    virtual int64_t
    get_fe_workspace_size_node() const override final {
        auto const& q   = attributes.inputs.find(input_names::Q);
        int64_t const h = q->second->get_dim()[1];
        return h * sizeof(float);
    }

    virtual error_t
    pass_by_value_tensors_(
        cudnnHandle_t handle,
        std::unordered_map<std::shared_ptr<Tensor_attributes>, void*> const&,
        std::unordered_map<std::shared_ptr<Tensor_attributes>, pass_by_values_t>& tensor_to_pass_by_value,
        void* node_workspace) const override final {
        if (attributes.dropout_probability.has_value() && attributes.dropout_probability.value() != 0.0) {
#if CUDNN_VERSION < 8903
            half dropout_scale_value = (1.0f / (1.0f - attributes.dropout_probability.value()));
#else
            float dropout_scale_value = (1.0f / (1.0f - attributes.dropout_probability.value()));
#endif
            tensor_to_pass_by_value.emplace(dropout_scale, dropout_scale_value);
        }

        if (attributes.padding_mask) {
            float negative_inf_value = std::numeric_limits<float>::lowest();
            tensor_to_pass_by_value.emplace(negative_inf_padding, negative_inf_value);
        }

        if (attributes.causal_mask) {
            float negative_inf_value = std::numeric_limits<float>::lowest();
            tensor_to_pass_by_value.emplace(negative_inf_causal, negative_inf_value);
        }

        if (attributes.alibi_mask) {
            CUDNN_FE_VALIDATE_AND_ASSIGN_INPUT_TENSOR(Q, input_names::Q);
            int64_t const h            = Q->second->get_dim()[1];
            auto h_alibi_slopes_vector = detail::get_abili_slope(h);

            cudaStream_t stream;
            CHECK_CUDNN_ERROR(cudnnGetStream(handle, &stream));
            CHECK_CUDA_ERROR(cudaMemcpyAsync(
                node_workspace, h_alibi_slopes_vector.data(), h * sizeof(float), cudaMemcpyHostToDevice, stream));
            tensor_to_pass_by_value.emplace(alibi_slopes, node_workspace);
        }

        if (attributes.attn_scale_value.has_value()) {
            CUDNN_FE_VALIDATE_AND_ASSIGN_INPUT_TENSOR(Attn_scale, input_names::Attn_scale);
            tensor_to_pass_by_value.emplace(Attn_scale->second, attributes.attn_scale_value.value());
        }

        return {error_code_t::OK, ""};
    }
};

class ScaledDotProductFlashAttentionBackwardNode : public INode {
   private:
    // non-virtual node cpu tensors
    std::shared_ptr<Tensor_attributes> one_tensor;
    std::shared_ptr<Tensor_attributes> negative_inf_padding;
    std::shared_ptr<Tensor_attributes> negative_inf_causal;

    // non-virtual node gpu tensors
    std::shared_ptr<Tensor_attributes> dQ_accum;
    int64_t dQ_accum_size = 0;
    std::shared_ptr<Tensor_attributes> softmax_sum;
    int64_t softmax_sum_size = 0;
    std::shared_ptr<Tensor_attributes> alibi_slopes;
    int64_t alibi_slopes_size = 0;

   public:
    Scaled_dot_product_flash_attention_backward_attributes attributes;

    ScaledDotProductFlashAttentionBackwardNode(Scaled_dot_product_flash_attention_backward_attributes&& attributes_,
                                               detail::Context const& context)
        : INode(context), attributes(std::move(attributes_)) {}

    Type
    getType() override final {
        return Type::COMPOSITE;
    }

    error_t
    pre_validate_node() const override final {
        getLogger() << "[cudnn_frontend] INFO: "
                    << "Validating ScaledDotProductFlashAttentionBackwardNode" << attributes.name << "..." << std::endl;

        using input_names  = Scaled_dot_product_flash_attention_backward_attributes::input_names;
        using output_names = Scaled_dot_product_flash_attention_backward_attributes::output_names;

        auto const& q    = attributes.inputs.find(input_names::Q);
        bool const has_q = (q != attributes.inputs.end()) && (q->second != nullptr);
        RETURN_CUDNN_FRONTEND_ERROR_IF(!has_q, error_code_t::ATTRIBUTE_NOT_SET, "Tensor input q not set");
        auto const& k    = attributes.inputs.find(input_names::K);
        bool const has_k = (k != attributes.inputs.end()) && (k->second != nullptr);
        RETURN_CUDNN_FRONTEND_ERROR_IF(!has_k, error_code_t::ATTRIBUTE_NOT_SET, "Tensor input k not set");
        auto const& v    = attributes.inputs.find(input_names::V);
        bool const has_v = (v != attributes.inputs.end()) && (v->second != nullptr);
        RETURN_CUDNN_FRONTEND_ERROR_IF(!has_v, error_code_t::ATTRIBUTE_NOT_SET, "Tensor input v not set");
        auto const& o    = attributes.inputs.find(input_names::O);
        bool const has_o = (o != attributes.inputs.end()) && (o->second != nullptr);
        RETURN_CUDNN_FRONTEND_ERROR_IF(!has_o, error_code_t::ATTRIBUTE_NOT_SET, "Tensor input o not set");
        auto const& dO    = attributes.inputs.find(input_names::dO);
        bool const has_dO = (dO != attributes.inputs.end()) && (dO->second != nullptr);
        RETURN_CUDNN_FRONTEND_ERROR_IF(!has_dO, error_code_t::ATTRIBUTE_NOT_SET, "Tensor input dO not set");
        auto const& stats    = attributes.inputs.find(input_names::Stats);
        bool const has_stats = (stats != attributes.inputs.end()) && (stats->second != nullptr);
        RETURN_CUDNN_FRONTEND_ERROR_IF(!has_stats, error_code_t::ATTRIBUTE_NOT_SET, "Tensor input stats not set");
        auto const& dQ    = attributes.outputs.find(output_names::dQ);
        bool const has_dQ = (dQ != attributes.outputs.end()) && (dQ->second != nullptr);
        RETURN_CUDNN_FRONTEND_ERROR_IF(!has_dQ, error_code_t::ATTRIBUTE_NOT_SET, "Tensor output dQ not set");
        auto const& dK    = attributes.outputs.find(output_names::dK);
        bool const has_dK = (dK != attributes.outputs.end()) && (dK->second != nullptr);
        RETURN_CUDNN_FRONTEND_ERROR_IF(!has_dK, error_code_t::ATTRIBUTE_NOT_SET, "Tensor output dK not set");
        auto const& dV    = attributes.outputs.find(output_names::dV);
        bool const has_dV = (dV != attributes.outputs.end()) && (dV->second != nullptr);
        RETURN_CUDNN_FRONTEND_ERROR_IF(!has_dV, error_code_t::ATTRIBUTE_NOT_SET, "Tensor output dV not set");

        bool last_dim_is_one = q->second->get_stride().back() == 1;
        last_dim_is_one &= k->second->get_stride().back() == 1;
        last_dim_is_one &= v->second->get_stride().back() == 1;
        last_dim_is_one &= o->second->get_stride().back() == 1;
        last_dim_is_one &= *(stats->second->get_stride().end() - 1) == 1;
        last_dim_is_one &= *(stats->second->get_stride().end() - 2) == 1;
        last_dim_is_one &= dQ->second->get_stride().back() == 1;
        last_dim_is_one &= dK->second->get_stride().back() == 1;
        last_dim_is_one &= dV->second->get_stride().back() == 1;
        last_dim_is_one &= dO->second->get_stride().back() == 1;
        RETURN_CUDNN_FRONTEND_ERROR_IF(
            !last_dim_is_one,
            error_code_t::GRAPH_NOT_SUPPORTED,
            "The stride for the last dimension corresponding to the hidden size per head should be 1");

        auto const& dropout_mask = attributes.inputs.find(input_names::Dropout_mask);
        auto const& seq_len_q    = attributes.inputs.find(input_names::SEQ_LEN_Q);
        auto const& seq_len_kv   = attributes.inputs.find(input_names::SEQ_LEN_KV);
        auto const& attn_scale   = attributes.inputs.find(input_names::Attn_scale);

        bool const has_dropout_mask = (dropout_mask != attributes.inputs.end()) && (dropout_mask->second != nullptr);
        RETURN_CUDNN_FRONTEND_ERROR_IF(
            attributes.dropout_probability.has_value() && has_dropout_mask,
            error_code_t::ATTRIBUTE_NOT_SET,
            "Using both, custom dropout mask and internal-mask generation using dropout probability, is ill-formed.");

        RETURN_CUDNN_FRONTEND_ERROR_IF(
            attributes.dropout_probability.has_value() && attributes.dropout_probability.value() == 1.0,
            error_code_t::ATTRIBUTE_NOT_SET,
            "Dropout probability cannot be 1 as corresponding scale wont be well formed.");

        bool const has_seq_len_q  = (seq_len_q != attributes.inputs.end()) && (seq_len_q->second != nullptr);
        bool const has_seq_len_kv = (seq_len_kv != attributes.inputs.end()) && (seq_len_kv->second != nullptr);

        RETURN_CUDNN_FRONTEND_ERROR_IF(attributes.padding_mask && (!has_seq_len_q || !has_seq_len_kv),
                                       error_code_t::ATTRIBUTE_NOT_SET,
                                       "Padding mask requires seq_len_q and seq_len_kv to be set.");

        RETURN_CUDNN_FRONTEND_ERROR_IF((!attributes.padding_mask) && (has_seq_len_q || has_seq_len_kv),
                                       error_code_t::ATTRIBUTE_NOT_SET,
                                       "seq_len_q and seq_len_kv needs to be set only if padding mask is enabled.");

        bool const has_attn_scale = (attn_scale != attributes.inputs.end()) && (attn_scale->second != nullptr);
        RETURN_CUDNN_FRONTEND_ERROR_IF(has_attn_scale && attributes.attn_scale_value.has_value(),
                                       error_code_t::ATTRIBUTE_NOT_SET,
                                       "attn_scale with tensor and value cannot be set at the same time.");

        RETURN_CUDNN_FRONTEND_ERROR_IF(context.get_intermediate_data_type() == DataType_t::NOT_SET,
                                       error_code_t::ATTRIBUTE_NOT_SET,
                                       "Intermediate tensor data type needs to be set as internal tensors require it.");

        auto it         = attributes.inputs.find(input_names::Q);
        auto q_dim      = it->second->get_dim();
        auto hidden_dim = q_dim[3];

        RETURN_CUDNN_FRONTEND_ERROR_IF((((hidden_dim <= 128) && (hidden_dim % 8 == 0)) == false),
                                       error_code_t::GRAPH_NOT_SUPPORTED,
                                       "Num hidden_dim shoud be less than 128 and hidden_dim should be multiple of 8");

        auto attn_mask = attributes.inputs.find(input_names::Bias);
        if (attn_mask != attributes.inputs.end() && attn_mask->second != nullptr) {
            auto attn_mask_dtype = attn_mask->second->get_data_type();
            RETURN_CUDNN_FRONTEND_ERROR_IF((attn_mask_dtype == DataType_t::BOOLEAN),
                                           error_code_t::GRAPH_NOT_SUPPORTED,
                                           "Attn mask data type cannot be boolean");
        }

        CHECK_CUDNN_FRONTEND_ERROR(attributes.validate_inputs());
        return {error_code_t::OK, ""};
    }

    error_t
    post_validate_node() const override final {
        // Validate outputs
        // All properties of output tensors should have been set now.
        CHECK_CUDNN_FRONTEND_ERROR(attributes.validate_outputs());

        return {error_code_t::OK, ""};
    }

    error_t
    expand_and_infer_properties() override final {
        getLogger() << "[cudnn_frontend] INFO: Inferrencing properties for ScaledDotProductFlashAttentionBackwardNode "
                    << attributes.name << "..." << std::endl;

        using input_names  = Scaled_dot_product_flash_attention_backward_attributes::input_names;
        using output_names = Scaled_dot_product_flash_attention_backward_attributes::output_names;

        attributes.fill_from_context(context);

        // Gather dims to fill properties of virtual tensors
        auto const& q_dim = attributes.inputs[input_names::Q]->get_dim();
        auto b            = q_dim[0];
        auto h            = q_dim[1];
        auto s_q          = q_dim[2];
        auto d            = q_dim[3];
        auto const& k_dim = attributes.inputs[input_names::K]->get_dim();
        auto s_kv         = k_dim[2];

        // cuDNN frontend API attention requires Q, K, V where
        // Q = {b, h, s_q, d}
        // K = {b, h, s_kv, d}
        // V = {b, h, s_kv, d}
        // but cuDNN backend API attention requires Q, KT, VT
        // Q = {b, h, s_q, d}
        // KT = {b, h, d, s_kv}
        // VT = {b, h, d, s_kv}
        // So the code below maps the K->KT and V->VT
        std::vector<int64_t> temp_vec;

        temp_vec = attributes.inputs[input_names::K]->get_dim();
        std::swap(temp_vec[2], temp_vec[3]);
        attributes.inputs[input_names::K]->set_dim(temp_vec);

        temp_vec = attributes.inputs[input_names::K]->get_stride();
        std::swap(temp_vec[2], temp_vec[3]);
        attributes.inputs[input_names::K]->set_stride(temp_vec);

        temp_vec = attributes.inputs[input_names::V]->get_dim();
        std::swap(temp_vec[2], temp_vec[3]);
        attributes.inputs[input_names::V]->set_dim(temp_vec);

        temp_vec = attributes.inputs[input_names::V]->get_stride();
        std::swap(temp_vec[2], temp_vec[3]);
        attributes.inputs[input_names::V]->set_stride(temp_vec);

        std::shared_ptr<Tensor_attributes> last_output, exp_s_output, dS_output, rng_output;

        // --------------Initialize and create tensors before creating nodes--------------------
        // one_tensor is needed for non-dropout graphs
        // one_tensor is passed by the node
        one_tensor = std::make_shared<Tensor_attributes>();
        one_tensor->set_is_virtual(false).set_is_pass_by_value(true);
        one_tensor->set_dim({1, 1, 1, 1}).set_stride({1, 1, 1, 1});
        one_tensor->set_data_type(DataType_t::FLOAT);

        if (attributes.attn_scale_value.has_value()) {
            attributes.inputs[input_names::Attn_scale] = std::make_shared<Tensor_attributes>();
            attributes.inputs[input_names::Attn_scale]->set_is_virtual(false).set_is_pass_by_value(true);
            attributes.inputs[input_names::Attn_scale]->set_dim({1, 1, 1, 1}).set_stride({1, 1, 1, 1});
            attributes.inputs[input_names::Attn_scale]->set_data_type(DataType_t::FLOAT);
        }

        // alibi_slopes is passed by the node
        if (attributes.alibi_mask) {
            alibi_slopes = std::make_shared<Tensor_attributes>();
            alibi_slopes->set_is_virtual(false);
            alibi_slopes->set_dim({1, h, 1, 1}).set_stride({h, h, 1, 1});
            alibi_slopes->set_data_type(DataType_t::FLOAT);
            alibi_slopes_size = h * sizeof(float);
        }

        // negative_inf_padding is passed by the node
        if (attributes.padding_mask) {
            negative_inf_padding = std::make_shared<Tensor_attributes>();
            negative_inf_padding->set_is_virtual(false).set_is_pass_by_value(true);
            negative_inf_padding->set_dim({1, 1, 1, 1}).set_stride({1, 1, 1, 1});
            negative_inf_padding->set_data_type(DataType_t::FLOAT);
        }

        // negative_inf_causal is passed by the node
        if (attributes.causal_mask) {
            negative_inf_causal = std::make_shared<Tensor_attributes>();
            negative_inf_causal->set_is_virtual(false).set_is_pass_by_value(true);
            negative_inf_causal->set_dim({1, 1, 1, 1}).set_stride({1, 1, 1, 1});
            negative_inf_causal->set_data_type(DataType_t::FLOAT);
        }

        // if dropout_prob is used, then the node passes scale and scale inverse
        // if dropout_mask is used, then the user passes scale and scale_inverse
        bool is_dropout_prob = (attributes.dropout_probability.has_value());
        bool is_dropout_mask = (attributes.inputs[input_names::Dropout_mask] != nullptr);
        if (is_dropout_prob) {
            attributes.inputs[input_names::Dropout_scale] = std::make_shared<Tensor_attributes>();
            attributes.inputs[input_names::Dropout_scale]->set_is_virtual(false).set_is_pass_by_value(true);
            attributes.inputs[input_names::Dropout_scale]->set_dim({1, 1, 1, 1}).set_stride({1, 1, 1, 1});
            attributes.inputs[input_names::Dropout_scale]->set_data_type(DataType_t::FLOAT);
            attributes.inputs[input_names::Dropout_scale_inv] = std::make_shared<Tensor_attributes>();
            attributes.inputs[input_names::Dropout_scale_inv]->set_is_virtual(false).set_is_pass_by_value(true);
            attributes.inputs[input_names::Dropout_scale_inv]->set_dim({1, 1, 1, 1}).set_stride({1, 1, 1, 1});
            attributes.inputs[input_names::Dropout_scale_inv]->set_data_type(DataType_t::FLOAT);
        }

        // ---------------------input tensor workarounds---------------------------

        // workspace optimization is only supported on
        // cudnn verision >= 8.9.5
        // device version >= hopper
        // sizeof(dp tensor) <= max_dp_workspace

        // CUDNN_FRONTEND_ATTN_DP_WORKSPACE_LIMIT=unset  - enable workspace opt. until the default 256MB limit.
        // CUDNN_FRONTEND_ATTN_DP_WORKSPACE_LIMIT=-1     - always enable workspace opt.
        // CUDNN_FRONTEND_ATTN_DP_WORKSPACE_LIMIT=0      - always disable workspace opt.
        // CUDNN_FRONTEND_ATTN_DP_WORKSPACE_LIMIT=n      - enable workspace opt. until the n byte limit
        bool use_workspace_opt = false;

        struct cudaDeviceProp prop;
        CHECK_CUDA_ERROR(cudaGetDeviceProperties(&prop, 0));
        if (cudnnGetVersion() >= 8905 && prop.major >= 9) {
            // default upper limit for workspace 256MB
            int64_t max_dp_workspace_bytes = 256 * 1024 * 1024;

            // allow setting the upper limit with envvars
            char* env_dp_workspace_limit_char = std::getenv("CUDNN_FRONTEND_ATTN_DP_WORKSPACE_LIMIT");
            if (env_dp_workspace_limit_char) {
                try {
                    std::string env_dp_workspace_limit_str(env_dp_workspace_limit_char);
                    max_dp_workspace_bytes = static_cast<int64_t>(std::stoll(env_dp_workspace_limit_str));
                } catch (...) {
                    RETURN_CUDNN_FRONTEND_ERROR_IF(true,
                                                   error_code_t::ATTRIBUTE_NOT_SET,
                                                   "Invalid argument for CUDNN_FRONTEND_ATTN_DP_WORKSPACE_LIMIT "
                                                   "(int64_t; in bytes)");
                }
            }

            int64_t workspace_s_q               = ((s_q + 64 - 1) / 64) * 64;
            int64_t workspace_s_kv              = ((s_kv + 64 - 1) / 64) * 64;
            int64_t required_dp_workspace_bytes = b * h * workspace_s_q * workspace_s_kv * 2;

            if (max_dp_workspace_bytes == -1) {
                use_workspace_opt = true;
            } else if (max_dp_workspace_bytes == 0) {
                use_workspace_opt = false;
            } else {
                use_workspace_opt = (required_dp_workspace_bytes <= max_dp_workspace_bytes);
            }
        }

        // non-virtual dQ_accum is how the backend API signals workspace optimization
        if (!use_workspace_opt) {
            dQ_accum = std::make_shared<Tensor_attributes>();
            dQ_accum->set_is_virtual(false);
            dQ_accum->set_dim({b, h, s_q, d}).set_stride({h * s_q * d, s_q * d, d, 1});
            dQ_accum->set_data_type(DataType_t::FLOAT).set_reordering_type(TensorReordering_t::F16x16);
            dQ_accum_size = b * h * s_q * d * sizeof(float);
        }

        // --------------RNG node--------------------

        if (is_dropout_prob) {
            if (attributes.outputs[output_names::RNG_DUMP] != nullptr) {
                rng_output = attributes.outputs[output_names::RNG_DUMP];
                rng(attributes.inputs[input_names::Seed],
                    attributes.inputs[input_names::Offset],
                    Rng_attributes()
                        .set_name("rng")
                        .set_distribution(RngDistribution_t::BERNOULLI)
                        .set_bernoulli_probability(1.0f - attributes.dropout_probability.value()),
                    rng_output);
            } else {
                rng_output = rng(attributes.inputs[input_names::Seed],
                                 attributes.inputs[input_names::Offset],
                                 Rng_attributes()
                                     .set_name("rng")
                                     .set_distribution(RngDistribution_t::BERNOULLI)
                                     .set_bernoulli_probability(1.0f - attributes.dropout_probability.value()));
                rng_output->set_dim({b, h, s_q, s_kv}).set_stride({h * s_q * s_kv, s_q * s_kv, s_kv, 1});
            }
        } else if (is_dropout_mask) {
            rng_output = attributes.inputs[input_names::Dropout_mask];
        }

        // --------------"dO * o => softmax_sum" chain--------------------

        // last_output = dO * O
        last_output = pointwise(attributes.inputs[input_names::dO],
                                attributes.inputs[input_names::O],
                                Pointwise_attributes().set_name("mul_dO_O").set_mode(PointwiseMode_t::MUL));
        last_output->set_dim({b, h, s_q, d}).set_stride({h * s_q * d, s_q * d, h * d, 1});

        // last_output = reduce(last_output, "b h sq d -> b h sq 1")
        last_output =
            reduction(last_output, Reduction_attributes().set_name("reduce_dO_o").set_mode(ReductionMode_t::ADD));
        last_output->set_dim({b, h, s_q, 1}).set_stride({h * s_q, s_q, 1, 1});

        // softmax_sum = last_output * dropout_scale
        last_output = pointwise(last_output,
                                attributes.inputs[input_names::Dropout_scale_inv]
                                    ? attributes.inputs[input_names::Dropout_scale_inv]
                                    : one_tensor,
                                Pointwise_attributes().set_name("scale_dropout_inv").set_mode(PointwiseMode_t::MUL));

        softmax_sum = last_output;

        // --------------"Q @ KT => exp_softmax => dV" chain--------------------

        // s = einsum(q, k, "b h sq d, b h skv d -> b h sq skv")
        last_output = matmul(attributes.inputs[input_names::Q],
                             attributes.inputs[input_names::K],
                             Matmul_attributes()
                                 .set_name("matmul_Q_KT")
                                 .set_m_override(attributes.inputs[input_names::SEQ_LEN_Q])
                                 .set_n_override(attributes.inputs[input_names::SEQ_LEN_KV]));
        last_output->set_dim({b, h, s_q, s_kv}).set_stride({h * s_q * s_kv, s_q * s_kv, s_kv, 1});

        // last_output = last_output * attention_scale
        if (attributes.inputs[input_names::Attn_scale]) {
            last_output = pointwise(last_output,
                                    attributes.inputs[input_names::Attn_scale],
                                    Pointwise_attributes().set_name("mul_s_attn_scale").set_mode(PointwiseMode_t::MUL));
        }

        // (optional) last_output = last_output + bias
        if (attributes.inputs[input_names::Bias]) {
            last_output = pointwise(last_output,
                                    attributes.inputs[input_names::Bias],
                                    Pointwise_attributes().set_name("add_bias").set_mode(PointwiseMode_t::ADD));
        }

        // (optional) last_output = last_output + alibi_mask
        if (attributes.alibi_mask) {
            auto row_idx_output = pointwise(last_output,
                                            Pointwise_attributes()
                                                .set_name("gen_row_idx_alibi")
                                                .set_mode(PointwiseMode_t::GEN_INDEX)
                                                .set_axis(2)
                                                .set_compute_data_type(DataType_t::INT32));
            row_idx_output->set_data_type(DataType_t::INT32);

            auto col_idx_output = pointwise(last_output,
                                            Pointwise_attributes()
                                                .set_name("gen_col_idx_alibi")
                                                .set_mode(PointwiseMode_t::GEN_INDEX)
                                                .set_axis(3)
                                                .set_compute_data_type(DataType_t::INT32));
            col_idx_output->set_data_type(DataType_t::INT32);

            auto sub_idx_output = pointwise(col_idx_output,
                                            row_idx_output,
                                            Pointwise_attributes()
                                                .set_name("sub_col_row_alibi")
                                                .set_mode(PointwiseMode_t::SUB)
                                                .set_compute_data_type(DataType_t::INT32));
            sub_idx_output->set_data_type(DataType_t::INT32);

            auto alibi_mask_output =
                pointwise(sub_idx_output,
                          alibi_slopes,
                          Pointwise_attributes().set_name("mul_slope_alibi").set_mode(PointwiseMode_t::MUL));

            last_output = pointwise(last_output,
                                    alibi_mask_output,
                                    Pointwise_attributes().set_name("add_alibi").set_mode(PointwiseMode_t::ADD));
        }

        // (optional) Apply padding mask
        if (attributes.padding_mask) {
            auto row_idx_output = pointwise(last_output,
                                            Pointwise_attributes()
                                                .set_name("gen_row_idx_padding")
                                                .set_mode(PointwiseMode_t::GEN_INDEX)
                                                .set_axis(2)
                                                .set_compute_data_type(DataType_t::INT32));
            row_idx_output->set_data_type(DataType_t::INT32);

            auto col_idx_output = pointwise(last_output,
                                            Pointwise_attributes()
                                                .set_name("gen_col_idx_padding")
                                                .set_mode(PointwiseMode_t::GEN_INDEX)
                                                .set_axis(3)
                                                .set_compute_data_type(DataType_t::INT32));
            col_idx_output->set_data_type(DataType_t::INT32);

            auto row_mask_output = pointwise(row_idx_output,
                                             attributes.inputs[input_names::SEQ_LEN_Q],
                                             Pointwise_attributes()
                                                 .set_name("lt_row_sq_padding")
                                                 .set_mode(PointwiseMode_t::CMP_LT)
                                                 .set_compute_data_type(DataType_t::BOOLEAN));
            row_mask_output->set_data_type(DataType_t::BOOLEAN);

            auto col_mask_output = pointwise(col_idx_output,
                                             attributes.inputs[input_names::SEQ_LEN_KV],
                                             Pointwise_attributes()
                                                 .set_name("lt_col_skv_padding")
                                                 .set_mode(PointwiseMode_t::CMP_LT)
                                                 .set_compute_data_type(DataType_t::BOOLEAN));
            col_mask_output->set_data_type(DataType_t::BOOLEAN);

            auto padding_mask_output = pointwise(row_mask_output,
                                                 col_mask_output,
                                                 Pointwise_attributes()
                                                     .set_name("and_row_col_padding")
                                                     .set_mode(PointwiseMode_t::LOGICAL_AND)
                                                     .set_compute_data_type(DataType_t::BOOLEAN));
            padding_mask_output->set_data_type(DataType_t::BOOLEAN);

            last_output =
                pointwise(last_output,
                          negative_inf_padding,
                          padding_mask_output,
                          Pointwise_attributes().set_name("select_padding").set_mode(PointwiseMode_t::BINARY_SELECT));
        }

        // Causal Mask DAG
        if (attributes.causal_mask) {
            auto row_idx_output = pointwise(last_output,
                                            Pointwise_attributes()
                                                .set_name("gen_row_idx_causal")
                                                .set_mode(PointwiseMode_t::GEN_INDEX)
                                                .set_axis(2)
                                                .set_compute_data_type(DataType_t::INT32));
            row_idx_output->set_data_type(DataType_t::INT32);

            auto col_idx_output = pointwise(last_output,
                                            Pointwise_attributes()
                                                .set_name("gen_col_idx_causal")
                                                .set_mode(PointwiseMode_t::GEN_INDEX)
                                                .set_axis(3)
                                                .set_compute_data_type(DataType_t::INT32));
            col_idx_output->set_data_type(DataType_t::INT32);

            auto causal_mask_output = pointwise(row_idx_output,
                                                col_idx_output,
                                                Pointwise_attributes()
                                                    .set_name("gt_row_col_causal")
                                                    .set_mode(PointwiseMode_t::CMP_GE)
                                                    .set_compute_data_type(DataType_t::BOOLEAN));
            causal_mask_output->set_data_type(DataType_t::BOOLEAN);

            last_output =
                pointwise(last_output,
                          negative_inf_causal,
                          causal_mask_output,
                          Pointwise_attributes().set_name("select_causal").set_mode(PointwiseMode_t::BINARY_SELECT));
        }

        // last_output = last_output - stats
        last_output = pointwise(last_output,
                                attributes.inputs[input_names::Stats],
                                Pointwise_attributes().set_name("sub_s_m").set_mode(PointwiseMode_t::SUB));

        // last_output = exp(last_output)
        last_output  = pointwise(last_output, Pointwise_attributes().set_name("exp_s").set_mode(PointwiseMode_t::EXP));
        exp_s_output = last_output;

        // (optional) last_output = last_output * dropout rng_output
        if (is_dropout_prob || is_dropout_mask) {
            last_output =
                pointwise(last_output,
                          rng_output,
                          Pointwise_attributes().set_name("mul_p_dropout_mask").set_mode(PointwiseMode_t::MUL));
        }

        // (optional) last_output = last_output * dropout_scale
        if (attributes.inputs[input_names::Dropout_scale]) {
            last_output =
                pointwise(last_output,
                          attributes.inputs[input_names::Dropout_scale],
                          Pointwise_attributes().set_name("mul_p_dropout_scale").set_mode(PointwiseMode_t::MUL));
        }

        // dV = einsum(p, dO, "b h sq skv", "b h sq d -> b h skv d")
        // as reshape + matmul
        last_output = reshape(last_output, Reshape_attributes().set_name("reshape_p"));
        last_output->set_dim({b, h, s_kv, s_q}).set_stride({h * s_q * s_kv, s_q * s_kv, 1, s_kv});
        last_output->set_data_type(context.get_io_data_type());

        matmul(last_output,
               attributes.inputs[input_names::dO],
               Matmul_attributes()
                   .set_name("matmul_pT_dO")
                   .set_m_override(attributes.inputs[input_names::SEQ_LEN_KV])
                   .set_k_override(attributes.inputs[input_names::SEQ_LEN_Q]),
               attributes.outputs[output_names::dV]);

        // --------------"dO @ VT => dS_output => dK" chain--------------------

        // dP = einsum(dO, v, "b h sq d, b h skv d -> b h sq skv")
        last_output = matmul(attributes.inputs[input_names::dO],
                             attributes.inputs[input_names::V],
                             Matmul_attributes()
                                 .set_name("matmul_dO_VT")
                                 .set_m_override(attributes.inputs[input_names::SEQ_LEN_Q])
                                 .set_k_override(attributes.inputs[input_names::SEQ_LEN_KV]));
        last_output->set_dim({b, h, s_q, s_kv}).set_stride({h * s_q * s_kv, s_q * s_kv, s_kv, 1});

        // last_output = last_output(dP) * mask
        last_output = pointwise(last_output,
                                (is_dropout_prob || is_dropout_mask) ? rng_output : one_tensor,
                                Pointwise_attributes().set_name("dP_dropout_mask").set_mode(PointwiseMode_t::MUL));

        // last_output = last_output - softmax_sum
        last_output = pointwise(last_output,
                                softmax_sum,
                                Pointwise_attributes().set_name("sub_dP_softmax_sum").set_mode(PointwiseMode_t::SUB));

        // last_output = last_output * exp_s_output
        last_output = pointwise(
            last_output, exp_s_output, Pointwise_attributes().set_name("mul_dP_exp_s").set_mode(PointwiseMode_t::MUL));

        // (optional) last_output = last_output * dropout_scale
        if (attributes.inputs[input_names::Dropout_scale]) {
            last_output =
                pointwise(last_output,
                          attributes.inputs[input_names::Dropout_scale],
                          Pointwise_attributes().set_name("mul_dS_dropout_scale").set_mode(PointwiseMode_t::MUL));
        }

        if (attributes.outputs[output_names::dBias]) {
            reduction(last_output,
                      Reduction_attributes().set_name("red_dP_dBias").set_mode(ReductionMode_t::ADD),
                      attributes.outputs[output_names::dBias]);
        }

        // (optional) last_output = last_output * bmm_scale
        if (attributes.inputs[input_names::Attn_scale]) {
            last_output =
                pointwise(last_output,
                          attributes.inputs[input_names::Attn_scale],
                          Pointwise_attributes().set_name("mul_dS_attn_scale").set_mode(PointwiseMode_t::MUL));
        }

        dS_output = last_output;

        // dK = einsum(dS, Q, "b h sq skv", "b h sq d -> b h skv d")
        // as reshape + matmul
        last_output = reshape(last_output, Reshape_attributes().set_name("reshape_dS"));
        last_output->set_dim({b, h, s_kv, s_q}).set_stride({h * s_q * s_kv, s_q * s_kv, 1, s_kv});
        last_output->set_data_type(context.get_io_data_type());

        matmul(last_output,
               attributes.inputs[input_names::Q],
               Matmul_attributes()
                   .set_name("matmul_dST_Q")
                   .set_m_override(attributes.inputs[input_names::SEQ_LEN_KV])
                   .set_k_override(attributes.inputs[input_names::SEQ_LEN_Q]),
               attributes.outputs[output_names::dK]);

        // --------------"dp_scaled @ K => dQ" chain--------------------

        auto const& kt_dim    = attributes.inputs[input_names::K]->get_dim();
        auto const& kt_stride = attributes.inputs[input_names::K]->get_stride();

        // dQ = einsum(dS, K, "b h sq skv, b h skv d -> b h sq d")
        // as reshape + matmul
        last_output = reshape(attributes.inputs[input_names::K], Reshape_attributes().set_name("reshape_k"));
        last_output->set_dim({kt_dim[0], kt_dim[1], kt_dim[3], kt_dim[2]})
            .set_stride({kt_stride[0], kt_stride[1], kt_stride[3], kt_stride[2]});

        matmul(dS_output,
               last_output,
               Matmul_attributes()
                   .set_name("matmul_dS_K")
                   .set_m_override(attributes.inputs[input_names::SEQ_LEN_Q])
                   .set_k_override(attributes.inputs[input_names::SEQ_LEN_KV]),
               (dQ_accum) ? dQ_accum : attributes.outputs[output_names::dQ]);

        if (dQ_accum) {
            pointwise(dQ_accum,
                      Pointwise_attributes().set_name("identity_dQ").set_mode(PointwiseMode_t::IDENTITY),
                      attributes.outputs[output_names::dQ]);
        }

        // ---------------------output tensor workarounds---------------------------

        // non-virtual softmax_sum is required for below cuDNN 8.9.5
        // non-virtual softmax_sum is passed by the node
        if (cudnnGetVersion() < 8905) {
            softmax_sum->set_is_virtual(false);
            softmax_sum->set_dim({b, h, s_q, 1});
            softmax_sum->set_data_type(DataType_t::FLOAT);
            softmax_sum_size = b * h * s_q * sizeof(float);
        }

        return {error_code_t::OK, ""};
    }

    virtual int64_t
    get_fe_workspace_size_node() const override final {
        // set in infer_properties_node()
        return alibi_slopes_size + dQ_accum_size + softmax_sum_size;
    }

    error_t
    pass_by_value_tensors_(
        cudnnHandle_t handle,
        std::unordered_map<std::shared_ptr<Tensor_attributes>, void*> const&,
        std::unordered_map<std::shared_ptr<Tensor_attributes>, pass_by_values_t>& tensor_to_pass_by_value,
        void* node_workspace) const override final {
        using input_names = Scaled_dot_product_flash_attention_backward_attributes::input_names;

        if (one_tensor) {
            tensor_to_pass_by_value.emplace(one_tensor, 1.0f);
        }

        if (attributes.attn_scale_value.has_value()) {
            CUDNN_FE_VALIDATE_AND_ASSIGN_INPUT_TENSOR(Attn_scale, input_names::Attn_scale);
            tensor_to_pass_by_value.emplace(Attn_scale->second, attributes.attn_scale_value.value());
        }

        if (attributes.alibi_mask) {
            CUDNN_FE_VALIDATE_AND_ASSIGN_INPUT_TENSOR(Q, input_names::Q);
            int64_t const h       = Q->second->get_dim()[1];
            auto alibi_slopes_vec = detail::get_abili_slope(h);

            cudaStream_t stream;
            CHECK_CUDNN_ERROR(cudnnGetStream(handle, &stream));
            CHECK_CUDA_ERROR(cudaMemcpyAsync(
                node_workspace, alibi_slopes_vec.data(), h * sizeof(float), cudaMemcpyHostToDevice, stream));
            tensor_to_pass_by_value.emplace(alibi_slopes, node_workspace);
            node_workspace = static_cast<char*>(node_workspace) + alibi_slopes_size;
        }

        if (attributes.padding_mask) {
            float negative_inf_value = std::numeric_limits<float>::lowest();
            tensor_to_pass_by_value.emplace(negative_inf_padding, negative_inf_value);
        }

        if (attributes.causal_mask) {
            float negative_inf_value = std::numeric_limits<float>::lowest();
            tensor_to_pass_by_value.emplace(negative_inf_causal, negative_inf_value);
        }

        if (attributes.dropout_probability.has_value()) {
            float dropout_scale_value     = 1.0f / (1.0f - attributes.dropout_probability.value());
            float dropout_scale_inv_value = (1.0f - attributes.dropout_probability.value());

            CUDNN_FE_VALIDATE_AND_ASSIGN_INPUT_TENSOR(Dropout_scale, input_names::Dropout_scale);
            tensor_to_pass_by_value.emplace(Dropout_scale->second, dropout_scale_value);

            CUDNN_FE_VALIDATE_AND_ASSIGN_INPUT_TENSOR(Dropout_scale_inv, input_names::Dropout_scale_inv);
            tensor_to_pass_by_value.emplace(Dropout_scale_inv->second, dropout_scale_inv_value);
        }

        if (dQ_accum && !dQ_accum->get_is_virtual()) {
            cudaStream_t stream;
            CHECK_CUDNN_ERROR(cudnnGetStream(handle, &stream));
            CHECK_CUDA_ERROR(cudaMemsetAsync(node_workspace, 0, dQ_accum_size, stream));
            tensor_to_pass_by_value.emplace(dQ_accum, node_workspace);
            node_workspace = static_cast<char*>(node_workspace) + dQ_accum_size;
        }

        if (softmax_sum && !softmax_sum->get_is_virtual()) {
            // There is no requirement for softmax_sum to be memset to 0
            tensor_to_pass_by_value.emplace(softmax_sum, node_workspace);
        }

        return {error_code_t::OK, ""};
    }
};

}  // namespace cudnn_frontend::graph