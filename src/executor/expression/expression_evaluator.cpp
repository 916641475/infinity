// Copyright(C) 2023 InfiniFlow, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

module;

module expression_evaluator;

import stl;
import base_expression;
import aggregate_expression;
import case_expression;
import cast_expression;
import column_expression;
import function_expression;
import reference_expression;
import value_expression;
import in_expression;
import data_block;
import column_vector;
import expression_state;
import status;
import third_party;
import infinity_exception;
import expression_type;
import bound_cast_func;
import logger;

namespace infinity {

void ExpressionEvaluator::Init(const DataBlock *input_data_block) { input_data_block_ = input_data_block; }

void ExpressionEvaluator::Execute(const SharedPtr<BaseExpression> &expr, SharedPtr<ExpressionState> &state, SharedPtr<ColumnVector> &output_column) {

    switch (expr->type()) {
        case ExpressionType::kAggregate:
            return Execute(std::static_pointer_cast<AggregateExpression>(expr), state, output_column);
        case ExpressionType::kCast:
            return Execute(std::static_pointer_cast<CastExpression>(expr), state, output_column);
        case ExpressionType::kCase:
            return Execute(std::static_pointer_cast<CaseExpression>(expr), state, output_column);
        case ExpressionType::kColumn:
            return Execute(std::static_pointer_cast<ColumnExpression>(expr), state, output_column);
        case ExpressionType::kFunction:
            return Execute(std::static_pointer_cast<FunctionExpression>(expr), state, output_column);
        case ExpressionType::kValue:
            return Execute(std::static_pointer_cast<ValueExpression>(expr), state, output_column);
        case ExpressionType::kReference:
            return Execute(std::static_pointer_cast<ReferenceExpression>(expr), state, output_column);
        case ExpressionType::kIn:
            return Execute(std::static_pointer_cast<InExpression>(expr), state, output_column);
        default: {
            UnrecoverableError(fmt::format("Unknown expression type: {}", expr->Name()));
        }
    }
}

void ExpressionEvaluator::Execute(const SharedPtr<AggregateExpression> &expr,
                                  SharedPtr<ExpressionState> &state,
                                  SharedPtr<ColumnVector> &output_column_vector) {
    if (in_aggregate_) {
        Status status = Status::RecursiveAggregate(expr->ToString());
        LOG_ERROR(status.message());
        RecoverableError(status);
    }
    in_aggregate_ = true;
    SharedPtr<ExpressionState> &child_state = state->Children()[0];
    SharedPtr<BaseExpression> &child_expr = expr->arguments()[0];
    // Create output chunk.
    // TODO: Now output chunk is pre-allocate memory in expression state
    // TODO: In the future, it can be implemented as on-demand allocation.
    SharedPtr<ColumnVector> &child_output_col = child_state->OutputColumnVector();
    this->Execute(child_expr, child_state, child_output_col);

    if (expr->aggregate_function_.return_type_ != *output_column_vector->data_type()) {
        Status status = Status::DataTypeMismatch(expr->aggregate_function_.return_type_.ToString(), output_column_vector->data_type()->ToString());
        LOG_ERROR(status.message());
        RecoverableError(status);
    }

    auto data_state = state->agg_state_;

    switch (state->agg_flag_) {
        case AggregateFlag::kUninitialized: {
            expr->aggregate_function_.init_func_(data_state);
            state->agg_flag_ = AggregateFlag::kRunning;
        }
        case AggregateFlag::kRunning: {
            expr->aggregate_function_.update_func_(data_state, child_output_col);
            break;
        }
        case AggregateFlag::kFinish: {
            expr->aggregate_function_.update_func_(data_state, child_output_col);
            const_ptr_t result_ptr = expr->aggregate_function_.finalize_func_(data_state);
            output_column_vector->AppendByPtr(result_ptr);
            break;
        }
        case AggregateFlag::kRunAndFinish: {
            expr->aggregate_function_.init_func_(data_state);
            expr->aggregate_function_.update_func_(data_state, child_output_col);
            const_ptr_t result_ptr = expr->aggregate_function_.finalize_func_(data_state);
            output_column_vector->AppendByPtr(result_ptr);
            break;
        }
    }

    in_aggregate_ = false;
}

void ExpressionEvaluator::Execute(const SharedPtr<CastExpression> &expr,
                                  SharedPtr<ExpressionState> &state,
                                  SharedPtr<ColumnVector> &output_column_vector) {
    SharedPtr<ExpressionState> &child_state = state->Children()[0];
    SharedPtr<BaseExpression> &child_expr = expr->arguments()[0];
    // Create output chunk.
    // TODO: Now output chunk is pre-allocate memory in expression state
    // TODO: In the future, it can be implemented as on-demand allocation.
    SharedPtr<ColumnVector> &child_output = child_state->OutputColumnVector();
    Execute(child_expr, child_state, child_output);

    CastParameters cast_parameters;

    expr->func_.function(child_output, output_column_vector, child_output->Size(), cast_parameters);
}

void ExpressionEvaluator::Execute(const SharedPtr<CaseExpression> &, SharedPtr<ExpressionState> &, SharedPtr<ColumnVector> &) {
    UnrecoverableError("Case execution");
}

void ExpressionEvaluator::Execute(const SharedPtr<ColumnExpression> &, SharedPtr<ExpressionState> &, SharedPtr<ColumnVector> &) {
    UnrecoverableError("Column expression");
}

void ExpressionEvaluator::Execute(const SharedPtr<FunctionExpression> &expr,
                                  SharedPtr<ExpressionState> &state,
                                  SharedPtr<ColumnVector> &output_column_vector) {

    SizeT argument_count = expr->arguments().size();
    Vector<SharedPtr<ColumnVector>> arguments;
    arguments.reserve(argument_count);

    for (SizeT i = 0; i < argument_count; ++i) {
        SharedPtr<ExpressionState> &argument_state = state->Children()[i];
        SharedPtr<BaseExpression> &argument_expr = expr->arguments()[i];
        SharedPtr<ColumnVector> &argument_output = argument_state->OutputColumnVector();
        Execute(argument_expr, argument_state, argument_output);
        arguments.emplace_back(argument_output);
    }

    DataBlock func_input_data_block;
    func_input_data_block.Init(arguments);

    expr->func_.function_(func_input_data_block, output_column_vector);
}

void ExpressionEvaluator::Execute(const SharedPtr<ValueExpression> &expr,
                                  SharedPtr<ExpressionState> &,
                                  SharedPtr<ColumnVector> &output_column_vector) {
    // memory copy here.
    auto value = expr->GetValue();
    output_column_vector->SetValue(0, value);
    output_column_vector->Finalize(1);
}

void ExpressionEvaluator::Execute(const SharedPtr<ReferenceExpression> &expr,
                                  SharedPtr<ExpressionState> &,
                                  SharedPtr<ColumnVector> &output_column_vector) {
    SizeT column_index = expr->column_index();

    if (input_data_block_ == nullptr) {
        UnrecoverableError("Input data block is NULL");
    }
    if (column_index >= input_data_block_->column_count()) {
        UnrecoverableError("Invalid column index");
    }

    output_column_vector = input_data_block_->column_vectors[column_index];
}

void ExpressionEvaluator::Execute(const SharedPtr<InExpression> &, SharedPtr<ExpressionState> &, SharedPtr<ColumnVector> &) {
    Status status = Status::NotSupport("IN execution isn't implemented yet.");
    LOG_ERROR(status.message());
    RecoverableError(status);
}

} // namespace infinity
