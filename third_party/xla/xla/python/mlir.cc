/* Copyright 2021 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <string>

#include "llvm/Support/raw_ostream.h"
#include "mlir/Conversion/ReconcileUnrealizedCasts/ReconcileUnrealizedCasts.h"  // from @llvm-project
#include "mlir/Dialect/Func/Extensions/AllExtensions.h"  // from @llvm-project
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/Dialect/SparseTensor/IR/SparseTensor.h"  // from @llvm-project
#include "mlir/IR/BuiltinOps.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/Parser/Parser.h"  // from @llvm-project
#include "mlir/Pass/PassManager.h"  // from @llvm-project
#include "pybind11/cast.h"  // from @pybind11
#include "pybind11/pybind11.h"  // from @pybind11
#include "pybind11/stl.h"  // from @pybind11
#include "stablehlo/dialect/ChloOps.h"  // from @stablehlo
#include "stablehlo/dialect/Serialization.h"  // from @stablehlo
#include "stablehlo/dialect/StablehloOps.h"  // from @stablehlo
#include "xla/client/xla_computation.h"
#include "xla/mlir/utils/error_util.h"
#include "xla/mlir_hlo/_virtual_includes/mhlo_passes/mhlo/transforms/passes.h"
#include "xla/mlir_hlo/mhlo/IR/hlo_ops.h"
#include "xla/mlir_hlo/mhlo/transforms/passes.h"
#include "xla/pjrt/mlir_to_hlo.h"
#include "xla/python/refine_polymorphic_shapes.h"
#include "xla/python/status_casters.h"
#include "xla/python/types.h"
#include "xla/status.h"
#include "xla/translate/hlo_to_mhlo/hlo_to_mlir_hlo.h"
#include "tsl/platform/errors.h"

namespace py = pybind11;

namespace xla {
namespace {

StatusOr<mlir::OwningOpRef<mlir::ModuleOp>> ParseModule(
    mlir::MLIRContext* context, std::string str) {
  mlir::OwningOpRef<mlir::ModuleOp> module;
  context->loadDialect<mlir::func::FuncDialect>();
  context->loadDialect<mlir::mhlo::MhloDialect>();
  context->loadDialect<mlir::chlo::ChloDialect>();
  context->loadDialect<mlir::sparse_tensor::SparseTensorDialect>();
  context->loadDialect<mlir::stablehlo::StablehloDialect>();

  mlir::DialectRegistry registry;
  mlir::func::registerAllExtensions(registry);
  context->appendDialectRegistry(registry);

  mlir::BaseScopedDiagnosticHandler diagnostic_handler(context);
  module = mlir::parseSourceString<mlir::ModuleOp>(
      llvm::StringRef(str.data(), str.size()), context);
  if (!module) {
    return diagnostic_handler.ConsumeStatus();
  }
  if (failed(module->verifyInvariants())) {
    VLOG(1) << "MLIR verification failed.";
    module->dump();
    return diagnostic_handler.ConsumeStatus();
  }
  return module;
}

std::string PrintModule(mlir::ModuleOp module) {
  std::string s;
  llvm::raw_string_ostream os(s);
  mlir::OpPrintingFlags flags;
  flags.enableDebugInfo();
  module->print(os, flags);
  return s;
}

void EnablePrintBeforeAndAfter(mlir::PassManager& pm) {
  auto print_before = [](mlir::Pass*, mlir::Operation*) { return true; };
  auto print_after = [](mlir::Pass*, mlir::Operation*) { return true; };
  pm.enableIRPrinting(print_before, print_after);
}

// Converts an XlaComputation to an MHLO or StableHLO mlir::Module string.
// Exists for backwards compatibility.
// TODO(phawkins): port remaining users of XlaComputations to use mlir::Modules
// instead and delete this function.
StatusOr<std::string> PyXlaComputationToMlirModule(
    const XlaComputation& computation, bool emit_stable_hlo) {
  mlir::MLIRContext context;
  if (VLOG_IS_ON(3)) context.disableMultithreading();
  mlir::OwningOpRef<mlir::ModuleOp> module =
      mlir::ModuleOp::create(mlir::UnknownLoc::get(&context));
  context.loadDialect<mlir::func::FuncDialect>();
  context.loadDialect<mlir::mhlo::MhloDialect>();
  mlir::DialectRegistry registry;
  mlir::func::registerAllExtensions(registry);
  context.appendDialectRegistry(registry);

  TF_RETURN_IF_ERROR(ConvertHloToMlirHlo(*module, &computation.proto(),
                                         /*import_all_computations=*/true));
  mlir::PassManager pm(&context);
  if (VLOG_IS_ON(3)) EnablePrintBeforeAndAfter(pm);
  if (emit_stable_hlo) {
    pm.addPass(mlir::mhlo::createHloLegalizeToStablehloPass());
  }
  if (!mlir::succeeded(pm.run(*module))) {
    return tsl::errors::InvalidArgument("MHLO => StableHLO failed");
  }
  return PrintModule(*module);
}

StatusOr<XlaComputation> PyMlirModuleToXlaComputation(std::string mlir_module,
                                                      bool use_tuple_args,
                                                      bool return_tuple) {
  mlir::MLIRContext context;
  TF_ASSIGN_OR_RETURN(mlir::OwningOpRef<mlir::ModuleOp> module,
                      ParseModule(&context, mlir_module));
  XlaComputation computation;
  TF_RETURN_IF_ERROR(
      MlirToXlaComputation(*module, computation, use_tuple_args, return_tuple));
  return computation;
}

StatusOr<std::string> PyMhloToStablehlo(std::string mlir_module) {
  mlir::MLIRContext context;
  if (VLOG_IS_ON(3)) context.disableMultithreading();
  // JAX can be customized in a way that involves operations from custom
  // dialects showing up in JAX IR.
  // `ParseModule` won't know about these dialects, but that's fine since we
  // just want to convert MHLO ops to StableHLO ops here and leave everything
  // else unchanged.
  // In order to achieve that, we're allowing unregistered dialects here.
  context.allowUnregisteredDialects(true);
  TF_ASSIGN_OR_RETURN(mlir::OwningOpRef<mlir::ModuleOp> module,
                      ParseModule(&context, mlir_module));
  mlir::PassManager pm(&context);
  if (VLOG_IS_ON(3)) EnablePrintBeforeAndAfter(pm);
  pm.addPass(mlir::mhlo::createHloLegalizeToStablehloPass());
  if (!mlir::succeeded(pm.run(*module))) {
    return tsl::errors::InvalidArgument("MHLO => StableHLO failed");
  }
  return PrintModule(*module);
}

StatusOr<std::string> PyStablehloToMhlo(std::string mlir_module) {
  mlir::MLIRContext context;
  if (VLOG_IS_ON(3)) context.disableMultithreading();
  // See PyMhloToStablehlo for an explanation of why we're allowing unregistered
  // dialects here.
  context.allowUnregisteredDialects(true);
  TF_ASSIGN_OR_RETURN(mlir::OwningOpRef<mlir::ModuleOp> module,
                      ParseModule(&context, mlir_module));
  mlir::PassManager pm(&context);
  if (VLOG_IS_ON(3)) EnablePrintBeforeAndAfter(pm);
  pm.addPass(mlir::mhlo::createStablehloLegalizeToHloPass());
  if (!mlir::succeeded(pm.run(*module))) {
    return tsl::errors::InvalidArgument("StableHLO => MHLO failed");
  }
  return PrintModule(*module);
}

StatusOr<py::bytes> PySerializePortableArtifact(std::string mlir_module,
                                                std::string target) {
  mlir::MLIRContext context;
  if (VLOG_IS_ON(3)) context.disableMultithreading();
  TF_ASSIGN_OR_RETURN(mlir::OwningOpRef<mlir::ModuleOp> module,
                      ParseModule(&context, mlir_module));

  // Legalize CHLO -> [MHLO+Shape] -> StableHLO
  mlir::PassManager pm(&context);
  if (VLOG_IS_ON(3)) EnablePrintBeforeAndAfter(pm);
  pm.addNestedPass<mlir::func::FuncOp>(
      mlir::mhlo::createChloLegalizeToHloPass());
  pm.addNestedPass<mlir::func::FuncOp>(
      mlir::mhlo::createShapeLegalizeToHloPass());
  pm.addPass(mlir::createReconcileUnrealizedCastsPass());
  pm.addPass(mlir::mhlo::createHloLegalizeToStablehloPass());
  if (!mlir::succeeded(pm.run(*module))) {
    return tsl::errors::InvalidArgument(
        "CHLO => [MHLO+Shape] => StableHLO failed");
  }

  // Serialize portable artifact
  std::string buffer;
  llvm::raw_string_ostream os(buffer);
  if (failed(mlir::stablehlo::serializePortableArtifact(*module, target, os)))
    return tsl::errors::InvalidArgument("Failed to serialize StableHLO");
  return py::bytes(buffer);
}

StatusOr<std::string> PyDeserializePortableArtifact(std::string bytecode_str) {
  mlir::MLIRContext context;
  mlir::OwningOpRef<mlir::ModuleOp> module =
      mlir::stablehlo::deserializePortableArtifact(bytecode_str, &context);
  if (!module)
    return tsl::errors::InvalidArgument("Failed to deserialize StableHLO");
  return PrintModule(*module);
}

}  // namespace

void BuildMlirSubmodule(py::module& m) {
  py::module mlir_module = m.def_submodule("mlir", "MLIR/XLA integration");

  mlir_module.def("xla_computation_to_mlir_module",
                  xla::ValueOrThrowWrapper(PyXlaComputationToMlirModule),
                  py::arg("computation"), py::arg("emit_stable_hlo") = true);
  mlir_module.def("mlir_module_to_xla_computation",
                  xla::ValueOrThrowWrapper(PyMlirModuleToXlaComputation),
                  py::arg("mlir_module"), py::arg("use_tuple_args") = false,
                  py::arg("return_tuple") = false);
  mlir_module.def("mhlo_to_stablehlo",
                  xla::ValueOrThrowWrapper(PyMhloToStablehlo),
                  py::arg("mlir_module"));
  mlir_module.def("stablehlo_to_mhlo",
                  xla::ValueOrThrowWrapper(PyStablehloToMhlo),
                  py::arg("mlir_module"));
  mlir_module.def("serialize_portable_artifact",
                  xla::ValueOrThrowWrapper(PySerializePortableArtifact),
                  py::arg("mlir_module"), py::arg("target"));
  mlir_module.def("deserialize_portable_artifact",
                  xla::ValueOrThrowWrapper(PyDeserializePortableArtifact),
                  py::arg("mlir_module"));
  mlir_module.def(
      "refine_polymorphic_shapes",
      [](std::string mlir_module, bool enable_shape_assertions,
         bool validate_static_shapes) -> py::bytes {
        std::string buffer;
        llvm::raw_string_ostream os(buffer);
        xla::ThrowIfError(RefinePolymorphicShapes(
            mlir_module, os, enable_shape_assertions, validate_static_shapes));
        return py::bytes(buffer);
      },
      py::arg("mlir_module"), py::arg("enable_shape_assertions") = true,
      py::arg("validate_static_shapes") = true,
      R"(Refines the dynamic shapes for a module.
        The "main" function must have static shapes and all the
        intermediate dynamic shapes depend only on the input static
        shapes. Optionally, also validates that the resulting module has
        only static shapes.
      )");
}

}  // namespace xla
