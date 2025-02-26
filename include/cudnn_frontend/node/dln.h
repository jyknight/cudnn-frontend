#pragma once

#include "../../cudnn_frontend_Heuristics.h"
#include "../../cudnn_frontend_Logging.h"

#include "../graph_helpers.h"
#include "../node_interface.h"

namespace cudnn_frontend {

namespace graph {

class DLNNode : public INode {
    // Keep epsilon for pre-8906
    std::shared_ptr<Tensor_attributes> epsilon;

   public:
    Layernorm_backward_attributes attributes;

    DLNNode(Layernorm_backward_attributes&& attributes_, detail::Context const& context)
        : INode(context), attributes(std::move(attributes_)) {}

    Type
    getType() override final {
        return Type::DLN;
    }

    error_t
    pre_validate_node() const override final {
        getLogger() << "[cudnn_frontend] INFO: "
                    << "Validating DLNNode " << attributes.name << "..." << std::endl;

        CHECK_CUDNN_FRONTEND_ERROR(attributes.validate_inputs());

        return {error_code_t::OK, ""};
    }

    error_t
    expand_and_infer_properties() override final {
        getLogger() << "[cudnn_frontend] INFO: Inferencing properties for DLN node " << attributes.name << "..."
                    << std::endl;

        attributes.fill_from_context(context);

        // TODO: Only inferencing from X works today.
        auto X                  = attributes.inputs[Layernorm_backward_attributes::input_names::X];
        auto const x_tensor_dim = X->get_dim();

        auto DY            = attributes.inputs[Layernorm_backward_attributes::input_names::DY];
        auto dy_tensor_dim = DY->get_dim();

        // Only infer dims and strides if user did not set them
        if (dy_tensor_dim.empty()) {
            dy_tensor_dim.resize(x_tensor_dim.size());
            DY->set_dim(x_tensor_dim);
        }
        if (DY->get_stride().empty()) {
            auto const& DY_dim = DY->get_dim();
            // Default to NHWC
            auto const& stride_order = detail::generate_NHWC_stride_order(DY_dim.size());
            DY->set_stride(detail::generate_stride(DY_dim, stride_order));
        }

        auto DX            = attributes.outputs[Layernorm_backward_attributes::output_names::DX];
        auto dx_tensor_dim = DX->get_dim();
        // Only infer dims and strides if user did not set them
        if (dx_tensor_dim.empty()) {
            dx_tensor_dim.resize(x_tensor_dim.size());
            DX->set_dim(x_tensor_dim);
        }
        if (DX->get_stride().empty()) {
            auto const& DX_dim = DX->get_dim();
            // Default to NHWC
            auto const& stride_order = detail::generate_NHWC_stride_order(DX_dim.size());
            DX->set_stride(detail::generate_stride(DX_dim, stride_order));
        }

        auto scale_bias_dim = X->get_dim();
        scale_bias_dim[0]   = 1;

        // Set channel length tensors
        auto infer_scale_bias_tensors = [&scale_bias_dim](std::shared_ptr<Tensor_attributes>& T) {
            auto tensor_dim = T->get_dim();
            // Only infer dims and strides if user did not set them
            if (tensor_dim.empty()) {
                T->set_dim(scale_bias_dim);
            }
            if (T->get_stride().empty()) {
                auto const& T_dim = T->get_dim();
                // Default to NHWC
                auto const& stride_order = detail::generate_NHWC_stride_order(T_dim.size());
                T->set_stride(detail::generate_stride(T_dim, stride_order));
            }
        };

        infer_scale_bias_tensors(attributes.outputs[Layernorm_backward_attributes::output_names::DSCALE]);
        infer_scale_bias_tensors(attributes.outputs[Layernorm_backward_attributes::output_names::DBIAS]);

        if (cudnnGetVersion() < 8906) {
            epsilon = std::make_shared<Tensor_attributes>();
            epsilon->set_is_pass_by_value(true)
                .set_dim({1, 1, 1, 1})
                .set_stride({1, 1, 1, 1})
                .set_data_type(DataType_t::FLOAT);
        }

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
    create_cudnn_tensors(int64_t& uid, std::unordered_map<int64_t, std::shared_ptr<cudnn_frontend::Tensor>>& tensors)
        const override final {
        getLogger() << "[cudnn_frontend] INFO: "
                    << "Building DLNNode tensors " << attributes.name << "..." << std::endl;

        for (auto const& [name, tensor] : attributes.inputs) {
            (void)name;
            if (tensor) {
                CHECK_CUDNN_FRONTEND_ERROR(create_cudnn_tensor(tensor, uid, tensors));
            }
        }
        for (auto const& [name, tensor] : attributes.outputs) {
            (void)name;
            if (tensor) {
                CHECK_CUDNN_FRONTEND_ERROR(create_cudnn_tensor(tensor, uid, tensors));
            }
        }

        if (epsilon) {
            CHECK_CUDNN_FRONTEND_ERROR(create_cudnn_tensor(epsilon, uid, tensors));
        }

        return {error_code_t::OK, ""};
    }

    error_t
    create_cudnn_operations(
        std::unordered_set<uid_t>& uids_involved_in_operations,
        std::vector<std::shared_ptr<cudnn_frontend::Operation>>& operations,
        std::unordered_map<int64_t, std::shared_ptr<cudnn_frontend::Tensor>>& tensors) const override final {
        getLogger() << "[cudnn_frontend] INFO: "
                    << "Building DLNNode operations " << attributes.name << "..." << std::endl;

#ifndef NV_CUDNN_DISABLE_EXCEPTION
        try {
#endif

            // Create the DLN operation.
            auto&& DLN_op_builder =
                cudnn_frontend::OperationBuilder(DescriptorType_t::OPERATION_NORM_BACKWARD_DESCRIPTOR);

            DLN_op_builder.setNormalizationMode(NormMode_t::LAYER_NORM);

            CUDNN_FE_VALIDATE_AND_ASSIGN_INPUT_TENSOR(X, Layernorm_backward_attributes::input_names::X);
            DLN_op_builder.setxDesc(*(tensors.at(X->second->get_uid())));

            CUDNN_FE_VALIDATE_AND_ASSIGN_INPUT_TENSOR(DY, Layernorm_backward_attributes::input_names::DY);
            DLN_op_builder.setdyDesc(*(tensors.at(DY->second->get_uid())));

            CUDNN_FE_VALIDATE_AND_ASSIGN_INPUT_TENSOR(SCALE, Layernorm_backward_attributes::input_names::SCALE);
            DLN_op_builder.setScale(*(tensors.at(SCALE->second->get_uid())));

            CUDNN_FE_VALIDATE_AND_ASSIGN_INPUT_TENSOR(MEAN, Layernorm_backward_attributes::input_names::MEAN);
            CUDNN_FE_VALIDATE_AND_ASSIGN_INPUT_TENSOR(INV_VARIANCE,
                                                      Layernorm_backward_attributes::input_names::INV_VARIANCE);
            DLN_op_builder.setSavedMeanAndInvVar(*(tensors.at(MEAN->second->get_uid())),
                                                 *(tensors.at(INV_VARIANCE->second->get_uid())));

            CUDNN_FE_VALIDATE_AND_ASSIGN_OUTPUT_TENSOR(DSCALE, Layernorm_backward_attributes::output_names::DSCALE);
            CUDNN_FE_VALIDATE_AND_ASSIGN_OUTPUT_TENSOR(DBIAS, Layernorm_backward_attributes::output_names::DBIAS);
            DLN_op_builder.setDScaleAndDBias(*(tensors.at(DSCALE->second->get_uid())),
                                             *(tensors.at(DBIAS->second->get_uid())));

            CUDNN_FE_VALIDATE_AND_ASSIGN_OUTPUT_TENSOR(DX, Layernorm_backward_attributes::output_names::DX);
            DLN_op_builder.setdxDesc(*(tensors.at(DX->second->get_uid())));

            if (epsilon) {
                DLN_op_builder.setEpsilonTensor(*(tensors.at(epsilon->get_uid())));
                uids_involved_in_operations.insert(epsilon->get_uid());
            }

            auto operation = DLN_op_builder.build();

            operations.push_back(std::make_shared<Operation_v8>(std::move(operation)));

#ifndef NV_CUDNN_DISABLE_EXCEPTION
        } catch (cudnn_frontend::cudnnException& e) {
            throw cudnnException(e.what(), e.getCudnnStatus());
        }
#endif

        auto const& non_virtual_uids = attributes.get_non_virtual_uids();
        uids_involved_in_operations.insert(non_virtual_uids.begin(), non_virtual_uids.end());
        return {error_code_t::OK, ""};
    }

    virtual void
    serialize(json& j) const override final {
        j = attributes;
    }

    error_t
    pass_by_value_tensors_(
        cudnnHandle_t,
        std::unordered_map<std::shared_ptr<Tensor_attributes>, void*> const&,
        std::unordered_map<std::shared_ptr<Tensor_attributes>, pass_by_values_t>& tensor_to_pass_by_value,
        void*) const override final {
        if (epsilon) {
            // can pass in any dummy value
            tensor_to_pass_by_value.emplace(epsilon, 0.0f);
        }
        return {error_code_t::OK, ""};
    }
};

}  // namespace graph

}  // namespace cudnn_frontend