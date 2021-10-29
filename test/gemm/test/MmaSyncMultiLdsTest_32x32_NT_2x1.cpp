/*******************************************************************************
 *
 * MIT License
 *
 * Copyright 2021 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *******************************************************************************/

#include <type_traits>

#include "GemmTest.h"
#include "KernelGenerator.h"
#include "LdsMappingUtil.h"
#include "detail/MmaSyncMultiLds.h"

struct TestParams : public CommonTestParams
{
    using Base = CommonTestParams;

    // Types: ALL + double
    // Block Sizes: 32 x 32 x BlockK
    // Layouts: NT
    using Types       = typename Base::TestTypes32x32;
    using BlockSizes  = std::tuple<std::tuple<I<32>, I<32>, I<8>>,
                                  std::tuple<I<32>, I<32>, I<16>>,
                                  std::tuple<I<32>, I<32>, I<32>>>;
    using Layouts     = typename Base::TestLayoutsNT;
    using LayoutsLds  = typename Base::TestLayoutTypes;
    using MappingsLds = typename Base::TestMappingsLds;
    using BlocksXY    = std::tuple<std::tuple<I<2>, I<1>>>;
    using KernelParams =
        typename CombineLists<Types, BlockSizes, Layouts, LayoutsLds, MappingsLds, BlocksXY>::
            Result;

    // Assemble the kernel generator
    // Kernel: MmaSyncMulti
    using GeneratorImpl   = MmaSyncMultiLdsGenerator;
    using KernelGenerator = KernelGenerator<KernelParams, GeneratorImpl>;

    // Sanity check for kernel generator
    static_assert(std::is_same<typename GeneratorImpl::ResultT, typename Base::KernelT>::value,
                  "Kernels from this generator do not match testing interface");

    static inline typename KernelGenerator::ResultT kernels()
    {
        return KernelGenerator::generate();
    }
};

// Test suite for unique parameterization
class MmaSyncMultiLdsTest32x32NT2x1 : public GemmTest
{
};

TEST_P(MmaSyncMultiLdsTest32x32NT2x1, RunKernel)
{
    this->RunKernel();
}

INSTANTIATE_TEST_SUITE_P(GemmKernelTests,
                         MmaSyncMultiLdsTest32x32NT2x1,
                         ::testing::Combine(::testing::ValuesIn(TestParams::kernels()),
                                            ::testing::ValuesIn(TestParams::threadBlocks()),
                                            ::testing::ValuesIn(TestParams::problemSizes()),
                                            ::testing::ValuesIn(TestParams::alphas()),
                                            ::testing::ValuesIn(TestParams::betas())));
