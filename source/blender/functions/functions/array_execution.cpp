#include <llvm/IR/TypeBuilder.h>

#include "FN_tuple_call.hpp"

#include "array_execution.hpp"

namespace FN {
namespace Functions {

ArrayExecution::~ArrayExecution()
{
}

ArrayExecution::ArrayExecution(SharedFunction function) : m_function(std::move(function))
{
  for (Type *type : m_function->input_types()) {
    m_input_sizes.append(type->extension<CPPTypeInfo>().size());
  }
  for (Type *type : m_function->output_types()) {
    m_output_sizes.append(type->extension<CPPTypeInfo>().size());
  }
}

/* Tuple Call Array Execution
 **********************************************/

class TupleCallArrayExecution : public ArrayExecution {
 public:
  TupleCallArrayExecution(SharedFunction function) : ArrayExecution(std::move(function))
  {
    BLI_assert(m_function->has_body<TupleCallBody>());
  }

  void call(ArrayRef<uint> indices,
            ArrayRef<void *> input_buffers,
            MutableArrayRef<void *> output_buffers,
            ExecutionContext &execution_context) override
  {
    uint input_amount = m_function->input_amount();
    uint output_amount = m_function->output_amount();

    BLI_assert(input_amount == input_buffers.size());
    BLI_assert(output_amount == output_buffers.size());

    TupleCallBody &body = m_function->body<TupleCallBody>();
    FN_TUPLE_CALL_ALLOC_TUPLES(body, fn_in, fn_out);

    for (uint index : indices) {
      for (uint i = 0; i < input_amount; i++) {
        void *ptr = POINTER_OFFSET(input_buffers[i], index * m_input_sizes[i]);
        fn_in.copy_in__dynamic(i, ptr);
      }

      body.call(fn_in, fn_out, execution_context);

      for (uint i = 0; i < output_amount; i++) {
        void *ptr = POINTER_OFFSET(output_buffers[i], index * m_output_sizes[i]);
        fn_out.relocate_out__dynamic(i, ptr);
      }
    }
  }
};

std::unique_ptr<ArrayExecution> get_tuple_call_array_execution(SharedFunction function)
{
  return std::unique_ptr<ArrayExecution>(new TupleCallArrayExecution(std::move(function)));
}

/* LLVM Array Execution
 ********************************************/

typedef void(CompiledFunctionSignature)(uint size,
                                        const uint *indices,
                                        void *const *input_buffers,
                                        void **output_buffers,
                                        void *context_ptr);

class LLVMArrayExecution : public ArrayExecution {
 private:
  Vector<LLVMTypeInfo *> m_input_type_infos;
  Vector<LLVMTypeInfo *> m_output_type_infos;
  std::unique_ptr<CompiledLLVM> m_compiled_function;

 public:
  LLVMArrayExecution(SharedFunction function) : ArrayExecution(std::move(function))
  {
    BLI_assert(m_function->has_body<LLVMBuildIRBody>());
    m_input_type_infos = m_function->input_extensions<LLVMTypeInfo>();
    m_output_type_infos = m_function->output_extensions<LLVMTypeInfo>();
    this->compile();
  }

  void call(ArrayRef<uint> indices,
            ArrayRef<void *> input_buffers,
            MutableArrayRef<void *> output_buffers,
            ExecutionContext &execution_context) override
  {
    CompiledFunctionSignature *function = (CompiledFunctionSignature *)
                                              m_compiled_function->function_ptr();
    function(indices.size(),
             indices.begin(),
             input_buffers.begin(),
             output_buffers.begin(),
             (void *)&execution_context);
  }

 private:
  void compile()
  {
    llvm::LLVMContext *context = aquire_llvm_context();
    llvm::Module *module = new llvm::Module(m_function->name() + " (Array Execution)", *context);
    llvm::Function *function = this->build_function_ir(module);
    m_compiled_function = CompiledLLVM::FromIR(module, function);
    release_llvm_context(context);
  }
  llvm::Function *build_function_ir(llvm::Module *module)
  {
    llvm::LLVMContext &context = module->getContext();
    LLVMBuildIRBody &body = m_function->body<LLVMBuildIRBody>();

    /* Create the main function. */
    llvm::FunctionType *ftype = llvm::TypeBuilder<CompiledFunctionSignature, false>::get(context);
    llvm::Function *function = llvm::Function::Create(
        ftype, llvm::GlobalValue::LinkageTypes::ExternalLinkage, module->getName(), module);
    llvm::BasicBlock *bb = llvm::BasicBlock::Create(context, "entry", function);
    CodeBuilder builder(bb);

    /* Take input parameters. */
    llvm::Value *size = builder.take_function_input(0, "indices_amount");
    llvm::Value *indices = builder.take_function_input(1, "indices");
    llvm::Value *input_buffers_arg = builder.take_function_input(2, "input_buffers");
    llvm::Value *output_buffers_arg = builder.take_function_input(3, "output_buffers");
    llvm::Value *context_ptr = builder.take_function_input(4, "context_ptr");

    /* Extract individual input and output buffers from parameters. */
    Vector<llvm::Value *> input_buffers = get_input_buffers(builder, input_buffers_arg);
    Vector<llvm::Value *> output_buffers = get_output_buffers(builder, output_buffers_arg);

    /* Create loop to iterate over all indices. */
    IRConstruct_IterationsLoop loop = builder.CreateNIterationsLoop(size, "Loop");
    CodeBuilder body_builder = loop.body_builder();
    llvm::Value *iteration = loop.current_iteration();
    llvm::Value *index_to_process = body_builder.CreateLoadAtIndex(indices, iteration);

    /* Load inputs for the current iteration into llvm. */
    Vector<llvm::Value *> input_values = this->get_input_values(
        body_builder, input_buffers, index_to_process);
    input_values.append(context_ptr);

    /* Call the actual function. */
    FunctionIRCache function_cache;
    BuildIRSettings settings;
    llvm::Function *actual_function = body.build_function(
        module, m_function->name(), settings, function_cache);
    llvm::Value *result = body_builder.CreateCall(actual_function, input_values);

    /* Store the computed results in the output buffers. */
    this->store_output_values(body_builder, output_buffers, index_to_process, result);

    /* TODO(jacques): check if input values have to be freed. */

    loop.finalize(builder);
    builder.CreateRetVoid();

    return function;
  }

  Vector<llvm::Value *> get_input_buffers(CodeBuilder &builder, llvm::Value *input_buffers_arg)
  {
    Vector<llvm::Value *> input_buffers;
    for (uint i = 0; i < m_function->input_amount(); i++) {
      uint element_size = m_input_sizes[i];
      llvm::Value *input_buffer = builder.CreateLoadAtIndex(input_buffers_arg, i);
      llvm::Value *typed_input_buffer = builder.CastToPointerWithStride(input_buffer,
                                                                        element_size);
      typed_input_buffer->setName(to_llvm(m_function->input_name(i) + " Array"));
      input_buffers.append(typed_input_buffer);
    }
    return input_buffers;
  }

  Vector<llvm::Value *> get_output_buffers(CodeBuilder &builder, llvm::Value *output_buffers_arg)
  {
    Vector<llvm::Value *> output_buffers;
    for (uint i = 0; i < m_function->output_amount(); i++) {
      uint element_size = m_output_sizes[i];
      llvm::Value *output_buffer = builder.CreateLoadAtIndex(output_buffers_arg, i);
      llvm::Value *typed_output_buffer = builder.CastToPointerWithStride(output_buffer,
                                                                         element_size);
      output_buffers.append(typed_output_buffer);
    }
    return output_buffers;
  }

  Vector<llvm::Value *> get_input_values(CodeBuilder &builder,
                                         ArrayRef<llvm::Value *> input_buffers,
                                         llvm::Value *index_to_process)
  {
    Vector<llvm::Value *> input_values;
    for (uint i = 0; i < m_function->input_amount(); i++) {
      llvm::Value *addr = builder.CreateGEP(input_buffers[i], index_to_process);
      llvm::Value *value = m_input_type_infos[i]->build_load_ir__copy(builder, addr);
      value->setName(to_llvm(m_function->input_name(i)));
      input_values.append(value);
    }
    return input_values;
  }

  void store_output_values(CodeBuilder &builder,
                           ArrayRef<llvm::Value *> output_buffers,
                           llvm::Value *index_to_process,
                           llvm::Value *computed_results)
  {
    for (uint i = 0; i < m_function->output_amount(); i++) {
      llvm::Value *addr = builder.CreateGEP(output_buffers[i], index_to_process);
      llvm::Value *value = builder.CreateExtractValue(computed_results, i);
      value->setName(to_llvm(m_function->output_name(i)));
      m_output_type_infos[i]->build_store_ir__relocate(builder, value, addr);
    }
  }
};

std::unique_ptr<ArrayExecution> get_precompiled_array_execution(SharedFunction function)
{
  return std::unique_ptr<ArrayExecution>(new LLVMArrayExecution(std::move(function)));
}

}  // namespace Functions
}  // namespace FN
