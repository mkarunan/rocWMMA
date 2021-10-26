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

#ifndef WMMA_DEVICE_MMA_SYNC_LDS_H
#define WMMA_DEVICE_MMA_SYNC_LDS_H

// The testing interface instantiates fp64 typed tests for all
// target devices. MI-100 mfma needs to be instantiated at compile time,
// but it doesn't do anything except provide a deprecation warning (e.g. not supported).
// A run-time check will abort the MI-100 fp64 tests anyway.
// Silence this warning for MmaSyncTests, as test coverage is needed
// for fp64 on all other targets which succeed MI-100.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include "WMMA.h"
#pragma GCC diagnostic pop

template <uint32_t BlockM,
          uint32_t BlockN,
          uint32_t BlockK,
          typename InputT,
          typename OutputT,
          typename ComputeT,
          typename LayoutA,
          typename LayoutB,
          typename LayoutC,
          typename LayoutD>
__global__ void __launch_bounds__(256, 1) mmaSyncLds(uint32_t       m,
                                                     uint32_t       n,
                                                     uint32_t       k,
                                                     InputT const*  a,
                                                     InputT const*  b,
                                                     OutputT const* c,
                                                     OutputT*       d,
                                                     uint32_t       lda,
                                                     uint32_t       ldb,
                                                     uint32_t       ldc,
                                                     uint32_t       ldd,
                                                     ComputeT       alpha,
                                                     ComputeT       beta)
{
    // Setup global mapping
    using MappingA = MappingUtil<BlockM, BlockK, InputT, LayoutA>;
    using MappingB = MappingUtil<BlockK, BlockN, InputT, LayoutB>;
    using MappingC = MappingUtil<BlockM, BlockN, OutputT, LayoutC>;
    using MappingD = MappingUtil<BlockM, BlockN, OutputT, LayoutD>;

    using FragA   = wmma::fragment<matrix_a, BlockM, BlockN, BlockK, InputT, LayoutA>;
    using FragB   = wmma::fragment<matrix_b, BlockM, BlockN, BlockK, InputT, LayoutB>;
    using FragC   = wmma::fragment<accumulator, BlockM, BlockN, BlockK, OutputT>;
    using FragAcc = wmma::fragment<accumulator, BlockM, BlockN, BlockK, ComputeT>;

    // Will store to LDS as though it were a register file.
    // Rows = register count
    // Cols = unpacked register elements = 64
    // Row major to minimize bank conflicts
    constexpr uint32_t registerFileWidth = AMDGCN_WAVE_SIZE;
    using MappingLdsA = MappingUtil<FragA::size(), registerFileWidth, InputT, row_major>;
    using MappingLdsB = MappingUtil<FragB::size(), registerFileWidth, InputT, row_major>;
    using FragLdsA
        = wmma::fragment<register_file, 1, registerFileWidth, FragA::size(), InputT, row_major>;
    using FragLdsB
        = wmma::fragment<register_file, 1, registerFileWidth, FragB::size(), InputT, row_major>;

    static_assert(FragA::size() * 64 == BlockM * BlockK, "Elements of A don't match");
    static_assert(FragLdsA::size() == FragA::size(), "A Sizes don't match");
    static_assert(FragB::size() * 64 == BlockK * BlockN, "Elements don't match");
    static_assert(FragLdsB::size() == FragB::size(), "Sizes don't match");

    // Target C / D block on 2D grid
    auto matrixCoordC = MappingC::matrixCoord();

    if(std::get<0>(matrixCoordC) < m && std::get<1>(matrixCoordC) < n && BlockK < k)
    {
        // Initialize accumulator
        auto fragAcc = FragAcc();
        wmma::fill_fragment(fragAcc, static_cast<ComputeT>(0));

        // Accumulate A * B
        if(alpha)
        {
            // Setup starting addresses
            auto* addrA = MappingA::dataCoord(a, lda, std::make_pair(std::get<0>(matrixCoordC), 0));
            auto* addrB = MappingB::dataCoord(b, ldb, std::make_pair(0, std::get<1>(matrixCoordC)));

            // Prefetch the first block from global memory
            auto fragA = FragA();
            auto fragB = FragB();
            wmma::load_matrix_sync(fragA, addrA, lda);
            wmma::load_matrix_sync(fragB, addrB, ldb);

            // Setup a register file in LDS which is friendly to minimizing bank conflicts.
            // Register file blocks in LDS follow same wg mapping for convenience.
            // Each wave will prefetch one block of A and one block of B.
            // A blocks occupy first portion of LDS and B blocks occupy the latter.
            HIP_DYNAMIC_SHARED(void*, localMemPtr);
            auto workgroupDim = MappingLdsA::workgroupDim();
            auto ldLds        = registerFileWidth * std::get<1>(workgroupDim);

            auto* baseAddrLdsA = reinterpret_cast<InputT*>(localMemPtr);
            auto* baseAddrLdsB
                = baseAddrLdsA + std::get<0>(workgroupDim) * FragLdsA::size() * ldLds;

            auto matrixCoordLdsA = MappingLdsA::matrixCoord(MappingLdsA::waveCoord());
            auto matrixCoordLdsB = MappingLdsB::matrixCoord(MappingLdsB::waveCoord());

            auto* addrLdsA = MappingLdsA::dataCoord(baseAddrLdsA, ldLds, matrixCoordLdsA);
            auto* addrLdsB = MappingLdsA::dataCoord(baseAddrLdsB, ldLds, matrixCoordLdsB);

            wmma::store_matrix_sync(addrLdsA, reinterpret_cast<FragLdsA&>(fragA), ldLds);
            wmma::store_matrix_sync(addrLdsB, reinterpret_cast<FragLdsB&>(fragB), ldLds);

            // Setup address increments.
            // A steps BlockK through m x k
            // B steps BlockK through k x n
            auto incrA = MappingA::dataOffset(lda, std::make_pair(0, BlockK));
            auto incrB = MappingB::dataOffset(ldb, std::make_pair(BlockK, 0));

            auto endA = addrA + incrA * (k / BlockK);

            addrA += incrA;
            addrB += incrB;

            while(addrA != endA)
            {
                // Keeping the workgroup in sync here is not necessary for correctness.
                // HOWEVER, if we keep waves in sync chances are good we may
                // benefit from cache hits on re-used data from A and B global loads.
                wmma::synchronize_workgroup();
                wmma::load_matrix_sync(reinterpret_cast<FragLdsA&>(fragA), addrLdsA, ldLds);
                wmma::load_matrix_sync(reinterpret_cast<FragLdsB&>(fragB), addrLdsB, ldLds);

                // Start pulling in the next block
                auto fragANext = FragA();
                auto fragBNext = FragB();
                wmma::load_matrix_sync(fragANext, addrA, lda);
                wmma::load_matrix_sync(fragBNext, addrB, ldb);

                // Mma for current block
                wmma::mma_sync(fragAcc, fragA, fragB, fragAcc);

                wmma::store_matrix_sync(addrLdsA, reinterpret_cast<FragLdsA&>(fragANext), ldLds);
                wmma::store_matrix_sync(addrLdsB, reinterpret_cast<FragLdsB&>(fragBNext), ldLds);

                addrA += incrA;
                addrB += incrB;
            }

            // Mma for the last block
            wmma::load_matrix_sync(reinterpret_cast<FragLdsA&>(fragA), addrLdsA, ldLds);
            wmma::load_matrix_sync(reinterpret_cast<FragLdsB&>(fragB), addrLdsB, ldLds);
            wmma::mma_sync(fragAcc, fragA, fragB, fragAcc);
        }

        // Load C
        auto fragC = FragC();
        wmma::fill_fragment(fragC, static_cast<OutputT>(0));
        if(beta)
        {
            // Setup address
            auto* addrC = MappingC::dataCoord(c, ldc, matrixCoordC);
            wmma::load_matrix_sync(fragC,
                                   addrC,
                                   ldc,
                                   std::is_same<LayoutC, row_major>::value ? wmma::mem_row_major
                                                                           : wmma::mem_col_major);
        }

        // D = alpha * accumAB + beta * C
#pragma unroll
        for(int i = 0; i < fragC.num_elements; ++i)
        {
            fragC.x[i] = OutputT(alpha * ComputeT(fragAcc.x[i]) + beta * ComputeT(fragC.x[i]));
        }

        // Output addresss
        auto* addrD = MappingD::dataCoord(d, ldd, matrixCoordC);

        // Store the output
        wmma::store_matrix_sync(addrD,
                                fragC,
                                ldd,
                                std::is_same<LayoutD, row_major>::value ? wmma::mem_row_major
                                                                        : wmma::mem_col_major);
    }
}

#endif // WMMA_DEVICE_MMA_SYNC_LDS_H