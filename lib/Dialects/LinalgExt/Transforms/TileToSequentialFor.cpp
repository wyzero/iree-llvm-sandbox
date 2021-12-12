//===- LowerToSCF.cpp.cpp - Lower to SCF ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===---------------------------------------------------------------------===//

#include "Dialects/LinalgExt/LinalgExtOps.h"
#include "Dialects/LinalgExt/PassDetail.h"
#include "Dialects/LinalgExt/Passes.h"
#include "Transforms/Utils.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Arithmetic/IR/Arithmetic.h"
#include "mlir/Dialect/Linalg/IR/LinalgOps.h"
#include "mlir/Dialect/SCF/SCF.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/IR/AffineExpr.h"
#include "mlir/IR/Identifier.h"
#include "mlir/IR/Operation.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "llvm/ADT/STLExtras.h"
#include <mlir/IR/BuiltinTypes.h>
#include <mlir/IR/OpDefinition.h>

using namespace mlir;
using namespace mlir::linalg_ext;

namespace {

/// Insert the `source` tensor into the `dest` tensor by creating the relevant
/// `subset_insert` op. The details of the `subset_insert` op are retrieved
/// from the `subset_extract` op so that they form a matching extract/insert
/// pair.
static Value createMatchingSubsetInsert(OpBuilder &b, Location loc,
                                        tensor::ExtractSliceOp subsetExtractOp,
                                        Value source, Value dest) {
  return b.create<tensor::InsertSliceOp>(
      loc, subsetExtractOp.source().getType(), source, dest,
      subsetExtractOp.offsets(), subsetExtractOp.sizes(),
      subsetExtractOp.strides(), subsetExtractOp.static_offsets(),
      subsetExtractOp.static_sizes(), subsetExtractOp.static_strides());
}

struct TileOpToSCFRewriter : public OpRewritePattern<linalg_ext::TileOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(linalg_ext::TileOp tileOp,
                                PatternRewriter &rewriter) const override {
    // TODO: verifier.
    assert(tileOp.getNumResults() > 0 &&
           tileOp.outs().size() == tileOp.getNumResults());

    // TODO: when supported, iterate over the tensor of sizes. This will be
    // iterating through a level of indirection.

    // Construct the loop bounds based on the canonical arithmetic progression.
    Location loc = tileOp.getLoc();
    Value zero = rewriter.create<arith::ConstantIndexOp>(loc, 0);
    Value one = rewriter.create<arith::ConstantIndexOp>(loc, 1);
    Value totalSize =
        rewriter.create<tensor::DimOp>(loc, tileOp.outs().front(), zero);
    Value step = tileOp.tile_sizes();
    assert(step.getType().isa<IndexType>() && "NYI: not an index type");

    // Construct the op without a body builder: we need to clone the ops in the
    // body explicitly after having access to the new bbArgs.
    // As a consequence, `ensureTerminator` is not called and the body has no
    // terminator.
    scf::ForOp forOp =
        rewriter.create<scf::ForOp>(loc, zero, totalSize, step, tileOp.outs());

    rewriter.setInsertionPointToStart(forOp.getBody());

    // TODO: when supported, also compute from the tensor of sizes.
    using AV = AffineValueExpr;
    AffineBuilder ab(rewriter, loc);
    AffineExpr i, j, M;
    bindDims(rewriter.getContext(), i, j);
    bindSymbols(rewriter.getContext(), M);

    // Materialize the implicit subtensors as explicit subset_extract.
    // TODO: geenralize to multiple offset/chunk_size bbargs if needed.
    // TODO: generalize the subset op.
    Value offset =
        ab.mul(AV(i).bind(forOp.getInductionVar()), AV(M).bind(step));
    // clang-format off
    Value size = ab.min(
      ValueRange{ab.sub(AV(i).bind(totalSize), AV(j).bind(offset)), 
      step});
    // clang-format on
    SmallVector<Value> implicitSubtensorExtracts;
    for (Value tensor : forOp.getRegionIterArgs()) {
      auto rankedTensorType = tensor.getType().cast<RankedTensorType>();
      SmallVector<Value> offsets(rankedTensorType.getRank(), zero);
      offsets.front() = offset;
      SmallVector<Value> sizes;
      sizes.push_back(size);
      for (int64_t i = 1, e = rankedTensorType.getRank(); i < e; ++i)
        sizes.push_back(rewriter.create<tensor::DimOp>(loc, tensor, i));
      SmallVector<Value> strides(rankedTensorType.getRank(), one);
      implicitSubtensorExtracts.push_back(
          rewriter.createOrFold<tensor::ExtractSliceOp>(loc, tensor, offsets,
                                                        sizes, strides));
    }

    // Regroup the values that replace the tileOp's bbArg and move the body.
    SmallVector<Value> bbArgsTranslated{offset, size};
    llvm::append_range(bbArgsTranslated, implicitSubtensorExtracts);
    rewriter.mergeBlocks(&tileOp.region().front(), forOp.getBody(),
                         bbArgsTranslated);
    // tileOp's terminator is not the terminator, insert explicit subset_insert
    // ops and feed them to a new scf.yield terminator that we can now add.
    auto tileYieldOp = cast<TileYieldOp>(&forOp.getBody()->back());
    SmallVector<Value> implicitSubtensorInserts;
    for (auto it :
         llvm::zip(implicitSubtensorExtracts, tileYieldOp.getOperands(),
                   forOp.getRegionIterArgs())) {
      implicitSubtensorInserts.push_back(createMatchingSubsetInsert(
          rewriter, loc,
          /*subsetExtractOp=*/
          std::get<0>(it).getDefiningOp<tensor::ExtractSliceOp>(),
          /*source=*/std::get<1>(it), /*dest=*/std::get<2>(it)));
    }
    // Insert terminator.
    rewriter.setInsertionPointToEnd(forOp.getBody());
    rewriter.create<scf::YieldOp>(loc, implicitSubtensorInserts);

    // Cleanup and replace.
    rewriter.eraseOp(tileYieldOp);
    rewriter.replaceOp(tileOp, forOp.getResults());

    return success();
  }
};

struct TileToSequentialForPass
    : public TileToSequentialForBase<TileToSequentialForPass> {
  void runOnOperation() override;
};
} // namespace

void TileToSequentialForPass::runOnOperation() {
  FuncOp funcOp = getOperation();
  MLIRContext *context = funcOp.getContext();
  RewritePatternSet patterns(context);
  patterns.insert<TileOpToSCFRewriter>(context);
  (void)applyPatternsAndFoldGreedily(funcOp, std::move(patterns));
}

std::unique_ptr<OperationPass<FuncOp>>
mlir::linalg_ext::createTileToSequentialForPass() {
  return std::make_unique<TileToSequentialForPass>();
}
