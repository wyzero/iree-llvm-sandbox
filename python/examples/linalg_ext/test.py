# RUN: %PYTHON %s 2>&1 | FileCheck %s

# This file contains simple test cases that combine various codegen options.

from ..core.experts import *
from ..core.harness import *
from ..core.transforms import *

from ..contraction.definitions import *

import typing as tp

################################################################################
# Compilation strategies.
################################################################################


def TestExpert(transforms: tp.Sequence[tp.Union[Transform,
                                                TransformationList]]):
  return (TransformationList(transforms=transforms) + Bufferize() +
          LoweringOnlyExpert('matmul_on_tensors', 'linalg.generic'))


expert_linalg_ext_tile = TestExpert([
    LinalgExtTile('matmul_on_tensors',
                  'linalg.generic',
                  tile_sizes=[2]),
    LinalgExtTileToSequentialFor('matmul_on_tensors',
                                 'linalg.generic'),
    #Vectorize('matmul_on_tensors', 'linalg.generic'),
])

all_experts = [
    e.print_ir(after_all=True, llvm=True) for e in [
        expert_linalg_ext_tile
    ]
]

################################################################################
# Problem instantiations.
################################################################################

keys = ['m', 'n', 'k']


def make_size_list(sizes: Sequence[int]):
    return {k: v for k, v in zip(keys, sizes)}


# CHECK-NOT: FAILURE
def main():
    n_iters = 1
    problem_size_list = [[3, 5, 7]]
    test_harness(lambda s, t: EinsumProblem('mk,kn'), [[np.float32] * 3],
                 map(make_size_list, problem_size_list),
                 all_experts,
                 n_iters=n_iters,
                 function_name='matmul_on_tensors')


if __name__ == '__main__':
    main()
