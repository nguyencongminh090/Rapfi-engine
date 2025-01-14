/*
 *  Rapfi, a Gomoku/Renju playing engine supporting piskvork protocol.
 *  Copyright (C) 2022  Rapfi developers
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mix8nnue.h"

#include "../core/iohelper.h"
#include "../core/platform.h"
#include "../core/utils.h"
#include "../game/board.h"
#include "simdops.h"
#include "weightloader.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

using namespace Evaluation::mix8;

constexpr auto Power3 = []() {
    auto pow3 = std::array<int, 16> {};
    for (size_t i = 0; i < pow3.size(); i++)
        pow3[i] = power(3, i);
    return pow3;
}();

constexpr int DX[4] = {1, 0, 1, 1};
constexpr int DY[4] = {0, 1, 1, -1};

constexpr int8_t Conv1dLine4Len9Points[33][2] = {
    {-4, -4}, {-4, 0}, {-4, 4}, {-3, -3}, {-3, 0}, {-3, 3}, {-2, -2}, {-2, 0}, {-2, 2},
    {-1, -1}, {-1, 0}, {-1, 1}, {0, -4},  {0, -3}, {0, -2}, {0, -1},  {0, 0},  {0, 1},
    {0, 2},   {0, 3},  {0, 4},  {1, -1},  {1, 0},  {1, 1},  {2, -2},  {2, 0},  {2, 2},
    {3, -3},  {3, 0},  {3, 3},  {4, -4},  {4, 0},  {4, 4},
};

static Evaluation::WeightRegistry<Mix8Weight> Mix8WeightRegistry;

struct Mix8BinaryWeightLoader : WeightLoader<Mix8Weight>
{
    std::unique_ptr<Mix8Weight> load(std::istream &in)
    {
        auto w = std::make_unique<Mix8Weight>();

        in.read(reinterpret_cast<char *>(w->mapping), sizeof(Mix8Weight::mapping));
        in.read(reinterpret_cast<char *>(w->map_prelu_weight),
                sizeof(Mix8Weight::map_prelu_weight));
        in.read(reinterpret_cast<char *>(w->feature_dwconv_weight),
                sizeof(Mix8Weight::feature_dwconv_weight));
        in.read(reinterpret_cast<char *>(w->feature_dwconv_bias),
                sizeof(Mix8Weight::feature_dwconv_bias));
        in.read(reinterpret_cast<char *>(&w->value_sum_scale_after_conv), sizeof(float));
        in.read(reinterpret_cast<char *>(&w->value_sum_scale_direct), sizeof(float));

        in.read(reinterpret_cast<char *>(&w->num_head_buckets), sizeof(int32_t));
        if (w->num_head_buckets < 1
            || w->num_head_buckets > MaxNumBuckets)  // can not hold all head buckets
            return nullptr;

        in.ignore(sizeof(Mix8Weight::__padding_to_64bytes_0));

        for (size_t i = 0; i < MaxNumBuckets; i++) {
            if (i < w->num_head_buckets)
                in.read(reinterpret_cast<char *>(&w->buckets[i]), sizeof(Mix8Weight::HeadBucket));
            else
                std::memset(reinterpret_cast<char *>(&w->buckets[i]),
                            0,
                            sizeof(Mix8Weight::HeadBucket));
        }

        if (in && in.peek() == std::ios::traits_type::eof())
            return w;
        else
            return nullptr;
    }
};

template <size_t Size, typename T>
using Batch = simd::detail::VecBatch<Size, T, simd::NativeInstType>;
template <typename FT, typename TT>
using Convert = simd::detail::VecCvt<FT, TT, simd::NativeInstType>;
using I16LS   = simd::detail::VecLoadStore<int16_t, mix8::Alignment, simd::NativeInstType>;
using I32LS   = simd::detail::VecLoadStore<int32_t, mix8::Alignment, simd::NativeInstType>;
using F32LS   = simd::detail::VecLoadStore<float, mix8::Alignment, simd::NativeInstType>;
using I16Op   = simd::detail::VecOp<int16_t, simd::NativeInstType>;
using I32Op   = simd::detail::VecOp<int32_t, simd::NativeInstType>;
using F32Op   = simd::detail::VecOp<float, simd::NativeInstType>;

}  // namespace

namespace Evaluation::mix8 {

Mix8Accumulator::Mix8Accumulator(int boardSize)
    : boardSize(boardSize)
    , fullBoardSize(boardSize + 2)
    , boardSizeScale(1.0f / (boardSize * boardSize))
{
    int nCells = boardSize * boardSize;
    indexTable = new std::array<uint32_t, 4>[nCells];
    mapSum     = MemAlloc::alignedArrayAlloc<std::array<int16_t, FeatureDim>, Alignment>(nCells);
    mapAfterDWConv = MemAlloc::alignedArrayAlloc<std::array<int16_t, FeatureDWConvDim>, Alignment>(
        fullBoardSize * fullBoardSize);

    std::fill_n(groupIndex, arraySize(groupIndex), 0);
    int size1 = (boardSize / 3) + (boardSize % 3 == 2);
    int size2 = (boardSize / 3) * 2 + (boardSize % 3 > 0);
    for (int i = 0; i < boardSize; i++)
        groupIndex[i] += (i >= size1) + (i >= size2);

    int groupSize[ValueSumType::NGroup][ValueSumType::NGroup] = {0};
    for (int y = 0; y < boardSize; y++)
        for (int x = 0; x < boardSize; x++)
            groupSize[groupIndex[y]][groupIndex[x]]++;
    for (int i = 0; i < ValueSumType::NGroup; i++)
        for (int j = 0; j < ValueSumType::NGroup; j++)
            groupSizeScale[i][j] = 1.0f / groupSize[i][j];
}

Mix8Accumulator::~Mix8Accumulator()
{
    delete[] indexTable;
    MemAlloc::alignedFree(mapSum);
    MemAlloc::alignedFree(mapAfterDWConv);
}

void Mix8Accumulator::initIndexTable()
{
    // Clear shape table
    std::fill_n(&indexTable[0][0], 4 * boardSize * boardSize, 0);

    auto getIndex = [&](int x, int y) -> std::array<uint32_t, 4> & {
        return indexTable[x + y * boardSize];
    };

    // Init shape table
    for (int thick = 1; thick <= 5; thick++) {
        for (int i = 0; i < boardSize; i++) {
            int c = 0;
            for (int j = 0; j < thick; j++)
                c += Power3[11 - j];

            getIndex(boardSize - 6 + thick, i)[0] = c;
            getIndex(i, boardSize - 6 + thick)[1] = c;
            getIndex(boardSize - 6 + thick, i)[2] = c;
            getIndex(i, boardSize - 6 + thick)[2] = c;
            getIndex(boardSize - 6 + thick, i)[3] = c;
            getIndex(i, 6 - 1 - thick)[3]         = c;
        }
    }

    for (int thick = 1; thick <= 5; thick++) {
        for (int i = 0; i < boardSize; i++) {
            int c = 2 * Power3[11];
            for (int j = 0; j < thick - 1; j++)
                c += Power3[j];

            getIndex(6 - 1 - thick, i)[0]         = c;
            getIndex(i, 6 - 1 - thick)[1]         = c;
            getIndex(6 - 1 - thick, i)[2]         = c;
            getIndex(i, 6 - 1 - thick)[2]         = c;
            getIndex(6 - 1 - thick, i)[3]         = c;
            getIndex(i, boardSize - 6 + thick)[3] = c;
        }
    }

    for (int a = 1; a <= 5; a++)
        for (int b = 1; b <= 5; b++) {
            int c = 3 * Power3[11];
            for (int i = 0; i < a - 1; i++)
                c += Power3[10 - i];
            for (int i = 0; i < b - 1; i++)
                c += Power3[i];

            getIndex(boardSize - 6 + a, 5 - b)[2]             = c;
            getIndex(5 - b, boardSize - 6 + a)[2]             = c;
            getIndex(5 - b, 5 - a)[3]                         = c;
            getIndex(boardSize - 6 + a, boardSize - 6 + b)[3] = c;
        }
}

void Mix8Accumulator::clear(const Mix8Weight &w)
{
    initIndexTable();

    // Init mapAfterDWConv to bias
    for (int i = 0; i < fullBoardSize * fullBoardSize; i++)
        simd::copy<FeatureDWConvDim>(mapAfterDWConv[i].data(), w.feature_dwconv_bias);
    // Init valueSum to zeros
    simd::zero<FeatureDim>(valueSum.global.data());
    for (int i = 0; i < ValueSumType::NGroup; i++)
        for (int j = 0; j < ValueSumType::NGroup; j++)
            simd::zero<FeatureDim>(valueSum.group[i][j].data());

    typedef Batch<FeatureDim, int16_t>       FeatB;
    typedef Batch<FeatureDWConvDim, int16_t> ConvB;
    typedef Batch<FeatureDim, int32_t>       VSumB;

    for (int y = 0, innerIdx = 0; y < boardSize; y++) {
        for (int x = 0; x < boardSize; x++, innerIdx++) {
            // Init mapSum from four directions
            simd::zero<FeatureDim>(mapSum[innerIdx].data());
            for (int dir = 0; dir < 4; dir++)
                simd::add<FeatureDim>(mapSum[innerIdx].data(),
                                      mapSum[innerIdx].data(),
                                      w.mapping[indexTable[innerIdx][dir]]);

            // Init mapAfterDWConv from mapSum
            for (int b = 0; b < FeatB::NumBatch; b++) {
                // Apply PReLU for mapSum
                auto feature = I16LS::load(mapSum[innerIdx].data() + b * FeatB::RegWidth);
                auto preluW  = I16LS::load(w.map_prelu_weight + b * FeatB::RegWidth);
                feature      = I16Op::max(feature, I16Op::mulhrs(feature, preluW));

                // Apply feature depthwise conv
                if (b < ConvB::NumBatch) {
                    for (int dy = 0; dy <= 2; dy++) {
                        int yi = y + dy;
                        for (int dx = 0; dx <= 2; dx++) {
                            int xi       = x + dx;
                            int outerIdx = xi + yi * fullBoardSize;

                            auto convWeight = I16LS::load(w.feature_dwconv_weight[8 - dy * 3 - dx]
                                                          + b * FeatB::RegWidth);
                            auto convPtr    = mapAfterDWConv[outerIdx].data() + b * FeatB::RegWidth;
                            auto convFeat   = I16LS::load(convPtr);
                            auto deltaFeat  = I16Op::mulhrs(feature, convWeight);
                            convFeat        = I16Op::add(convFeat, deltaFeat);
                            I16LS::store(convPtr, convFeat);
                        }
                    }
                }
                else {
                    auto [v0, v1] = Convert<int16_t, int32_t>::convert(feature);

                    // Add map feature to map value sum
                    auto addToAccumulator =
                        [&, v0_ = v0, v1_ = v1](std::array<int32_t, FeatureDim> &vSum) {
                            auto vSumPtr = vSum.data() + b * 2 * VSumB::RegWidth;
                            auto vSum0   = I32LS::load(vSumPtr);
                            auto vSum1   = I32LS::load(vSumPtr + VSumB::RegWidth);
                            vSum0        = I32Op::add(vSum0, v0_);
                            vSum1        = I32Op::add(vSum1, v1_);
                            I32LS::store(vSumPtr, vSum0);
                            I32LS::store(vSumPtr + VSumB::RegWidth, vSum1);
                        };
                    addToAccumulator(valueSum.global);
                    addToAccumulator(valueSum.group[groupIndex[y]][groupIndex[x]]);
                }
            }
        }
    }

    // Init valueSum by adding all dwconv value features
    for (int y = 0, outerIdx = fullBoardSize + 1; y < boardSize; y++, outerIdx += 2) {
        for (int x = 0; x < boardSize; x++, outerIdx++) {
            for (int b = 0; b < ConvB::NumBatch; b++) {
                auto feature  = I16LS::load(mapAfterDWConv[outerIdx].data() + b * ConvB::RegWidth);
                auto [v0, v1] = Convert<int16_t, int32_t>::convert(feature);
                v0            = I32Op::max(v0, I32Op::setzero());  // relu
                v1            = I32Op::max(v1, I32Op::setzero());  // relu

                auto addToAccumulator =
                    [&, v0_ = v0, v1_ = v1](std::array<int32_t, FeatureDim> &vSum) {
                        auto vSumPtr = vSum.data() + b * 2 * VSumB::RegWidth;
                        auto vSum0   = I32LS::load(vSumPtr);
                        auto vSum1   = I32LS::load(vSumPtr + VSumB::RegWidth);
                        vSum0        = I32Op::add(vSum0, v0_);
                        vSum1        = I32Op::add(vSum1, v1_);
                        I32LS::store(vSumPtr, vSum0);
                        I32LS::store(vSumPtr + VSumB::RegWidth, vSum1);
                    };
                addToAccumulator(valueSum.global);
                addToAccumulator(valueSum.group[groupIndex[y]][groupIndex[x]]);
            }
        }
    }
}

template <Mix8Accumulator::UpdateType UT>
void Mix8Accumulator::update(const Mix8Weight &w,
                             Color             pieceColor,
                             int               x,
                             int               y,
                             ValueSumType     *valueSumBoardBackup)
{
    assert(pieceColor == BLACK || pieceColor == WHITE);

    typedef Batch<FeatureDim, int16_t>       FeatB;
    typedef Batch<FeatureDWConvDim, int16_t> ConvB;
    typedef Batch<FeatureDim, int32_t>       VSumB;

    // Load value sum
    I32Op::R vSumGlobal[VSumB::NumBatch];
    I32Op::R vSumGroup[ValueSumType::NGroup][ValueSumType::NGroup][VSumB::NumBatch];
    int      x0, y0, x1, y1;
    if constexpr (UT == MOVE) {
        for (int b = 0; b < VSumB::NumBatch; b++)
            vSumGlobal[b] = I32LS::load(valueSum.global.data() + b * VSumB::RegWidth);
        for (int i = 0; i < ValueSumType::NGroup; i++)
            for (int j = 0; j < ValueSumType::NGroup; j++)
                for (int b = 0; b < VSumB::NumBatch; b++)
                    vSumGroup[i][j][b] =
                        I32LS::load(valueSum.group[i][j].data() + b * VSumB::RegWidth);

        x0 = std::max(x - 6 + 1, 1);
        y0 = std::max(y - 6 + 1, 1);
        x1 = std::min(x + 6 + 1, boardSize);
        y1 = std::min(y + 6 + 1, boardSize);

        // Subtract value feature sum
        for (int yi = y0, outerIdxBase = y0 * fullBoardSize; yi <= y1;
             yi++, outerIdxBase += fullBoardSize) {
            int i = groupIndex[yi - 1];
            for (int xi = x0; xi <= x1; xi++) {
                int outerIdx = xi + outerIdxBase;
                int j        = groupIndex[xi - 1];
                for (int b = 0; b < ConvB::NumBatch; b++) {
                    auto convF = I16LS::load(mapAfterDWConv[outerIdx].data() + b * ConvB::RegWidth);
                    convF      = I16Op::max(convF, I16Op::setzero());  // relu
                    auto [v0, v1] = Convert<int16_t, int32_t>::convert(convF);

                    const int offset            = 2 * b;
                    vSumGlobal[offset + 0]      = I32Op::sub(vSumGlobal[offset + 0], v0);
                    vSumGlobal[offset + 1]      = I32Op::sub(vSumGlobal[offset + 1], v1);
                    vSumGroup[i][j][offset + 0] = I32Op::sub(vSumGroup[i][j][offset + 0], v0);
                    vSumGroup[i][j][offset + 1] = I32Op::sub(vSumGroup[i][j][offset + 1], v1);
                }
            }
        }
    }

    struct OnePointChange
    {
        int8_t   x;
        int8_t   y;
        int16_t  dir;
        int16_t  innerIdx;
        uint32_t oldShape;
        uint32_t newShape;
    } changeTable[4 * 11];
    int changeCount = 0;
    int dPower3     = UT == MOVE ? pieceColor + 1 : -1 - pieceColor;

    // Update shape table and record changes
    int boardSizeSub1 = boardSize - 1;
    for (int dir = 0; dir < 4; dir++) {
        for (int dist = -5; dist <= 5; dist++) {
            int xi = x - dist * DX[dir];
            int yi = y - dist * DY[dir];

            // less-branch test: xi < 0 || xi >= boardSize || yi < 0 || yi >= boardSize
            if ((xi | (boardSizeSub1 - xi) | yi | (boardSizeSub1 - yi)) < 0)
                continue;

            OnePointChange &c           = changeTable[changeCount++];
            c.x                         = xi;
            c.y                         = yi;
            c.dir                       = dir;
            c.innerIdx                  = boardSize * yi + xi;
            c.oldShape                  = indexTable[c.innerIdx][dir];
            c.newShape                  = c.oldShape + dPower3 * Power3[dist + 5];
            indexTable[c.innerIdx][dir] = c.newShape;
            assert(0 <= c.newShape && c.newShape < ShapeNum);
        }
    }

    // Incremental update feature sum
    for (int i = 0; i < changeCount; i++) {
        const OnePointChange &c = changeTable[i];
        if (i + 1 < changeCount) {
            multiPrefetch<FeatureDim * sizeof(int16_t)>(w.mapping[changeTable[i + 1].oldShape]);
            multiPrefetch<FeatureDim * sizeof(int16_t)>(w.mapping[changeTable[i + 1].newShape]);
        }

        // Update mapSum and mapAfterDWConv
        I16Op::R oldFeats[FeatB::NumBatch];
        I16Op::R newFeats[FeatB::NumBatch];
        for (int b = 0; b < FeatB::NumBatch; b++) {
            // Update mapSum
            auto newMapFeat = I16LS::load(w.mapping[c.newShape] + b * FeatB::RegWidth);
            auto oldMapFeat = I16LS::load(w.mapping[c.oldShape] + b * FeatB::RegWidth);
            auto mapSumPtr  = mapSum[c.innerIdx].data() + b * FeatB::RegWidth;
            oldFeats[b]     = I16LS::load(mapSumPtr);
            newFeats[b]     = I16Op::sub(oldFeats[b], oldMapFeat);
            newFeats[b]     = I16Op::add(newFeats[b], newMapFeat);
            I16LS::store(mapSumPtr, newFeats[b]);

            // Apply PReLU for mapSum
            auto preluW = I16LS::load(w.map_prelu_weight + b * FeatB::RegWidth);
            oldFeats[b] = I16Op::max(oldFeats[b], I16Op::mulhrs(oldFeats[b], preluW));
            newFeats[b] = I16Op::max(newFeats[b], I16Op::mulhrs(newFeats[b], preluW));
        }

        // Update mapAfterDWConv
        for (int dy = 0, outerIdxBase = c.y * fullBoardSize + c.x; dy <= 2;
             dy++, outerIdxBase += fullBoardSize) {
            for (int dx = 0; dx <= 2; dx++) {
                auto *convWeightBase = w.feature_dwconv_weight[8 - dy * 3 - dx];
                auto *convBase       = mapAfterDWConv[dx + outerIdxBase].data();

                for (int b = 0; b < ConvB::NumBatch; b++) {
                    auto convPtr  = convBase + b * ConvB::RegWidth;
                    auto oldConvF = I16LS::load(convPtr);
                    auto convW    = I16LS::load(convWeightBase + b * ConvB::RegWidth);
                    auto newConvF = I16Op::sub(oldConvF, I16Op::mulhrs(oldFeats[b], convW));
                    newConvF      = I16Op::add(newConvF, I16Op::mulhrs(newFeats[b], convW));
                    I16LS::store(convPtr, newConvF);
                }
            }
        }

        if constexpr (UT == MOVE) {
            // Update valueSum
            for (int b = ConvB::NumBatch; b < FeatB::NumBatch; b++) {
                auto [oldv0, oldv1] = Convert<int16_t, int32_t>::convert(oldFeats[b]);
                auto [newv0, newv1] = Convert<int16_t, int32_t>::convert(newFeats[b]);

                auto addToAccumulator =
                    [b, ov0 = oldv0, ov1 = oldv1, nv0 = newv0, nv1 = newv1](auto &vSum) {
                        const int offset = 2 * b;
                        vSum[offset + 0] = I32Op::sub(vSum[offset + 0], ov0);
                        vSum[offset + 1] = I32Op::sub(vSum[offset + 1], ov1);
                        vSum[offset + 0] = I32Op::add(vSum[offset + 0], nv0);
                        vSum[offset + 1] = I32Op::add(vSum[offset + 1], nv1);
                    };
                addToAccumulator(vSumGlobal);
                addToAccumulator(vSumGroup[groupIndex[c.y]][groupIndex[c.x]]);
            }
        }
    }

    if constexpr (UT == MOVE) {
        // Add value feature sum
        for (int yi = y0, outerIdxBase = y0 * fullBoardSize; yi <= y1;
             yi++, outerIdxBase += fullBoardSize) {
            int i = groupIndex[yi - 1];
            for (int xi = x0; xi <= x1; xi++) {
                int outerIdx = xi + outerIdxBase;
                int j        = groupIndex[xi - 1];
                for (int b = 0; b < ConvB::NumBatch; b++) {
                    auto convF = I16LS::load(mapAfterDWConv[outerIdx].data() + b * ConvB::RegWidth);
                    convF      = I16Op::max(convF, I16Op::setzero());  // relu
                    auto [v0, v1] = Convert<int16_t, int32_t>::convert(convF);

                    const int offset            = 2 * b;
                    vSumGlobal[offset + 0]      = I32Op::add(vSumGlobal[offset + 0], v0);
                    vSumGlobal[offset + 1]      = I32Op::add(vSumGlobal[offset + 1], v1);
                    vSumGroup[i][j][offset + 0] = I32Op::add(vSumGroup[i][j][offset + 0], v0);
                    vSumGroup[i][j][offset + 1] = I32Op::add(vSumGroup[i][j][offset + 1], v1);
                }
            }
        }

        // Store value sum
        for (int b = 0; b < VSumB::NumBatch; b++)
            I32LS::store(valueSum.global.data() + b * VSumB::RegWidth, vSumGlobal[b]);
        for (int i = 0; i < ValueSumType::NGroup; i++)
            for (int j = 0; j < ValueSumType::NGroup; j++)
                for (int b = 0; b < VSumB::NumBatch; b++)
                    I32LS::store(valueSum.group[i][j].data() + b * VSumB::RegWidth,
                                 vSumGroup[i][j][b]);
    }
    else {
        valueSum = *valueSumBoardBackup;  // just copy it
    }
}

std::tuple<float, float, float> Mix8Accumulator::evaluateValue(const Mix8Weight &w)
{
    const auto &bucket = w.buckets[getBucketIndex()];

    typedef Batch<FeatureDim, float>       VSumB;
    typedef Batch<FeatureDWConvDim, float> ConvB;

    // convert value sum from int32 to float
    auto valueSumToFloat =
        [&w](float *output, const std::array<int32_t, FeatureDim> &vSum, float sizeScale) {
            auto scaleConv   = F32Op::set1(sizeScale * w.value_sum_scale_after_conv);
            auto scaleDirect = F32Op::set1(sizeScale * w.value_sum_scale_direct);
            for (int b = 0; b < ConvB::NumBatch; b++) {
                auto valueI32 = I32LS::load(vSum.data() + b * VSumB::RegWidth);
                auto valueF32 = Convert<int32_t, float>::convert1(valueI32);
                valueF32      = F32Op::mul(valueF32, scaleConv);
                F32LS::store(output + b * VSumB::RegWidth, valueF32);
            }
            for (int b = ConvB::NumBatch; b < VSumB::NumBatch; b++) {
                auto valueI32 = I32LS::load(vSum.data() + b * VSumB::RegWidth);
                auto valueF32 = Convert<int32_t, float>::convert1(valueI32);
                valueF32      = F32Op::mul(valueF32, scaleDirect);
                F32LS::store(output + b * VSumB::RegWidth, valueF32);
            }
        };

    alignas(Alignment) float layer0[FeatureDim + ValueGroupDim * 4];
    alignas(Alignment) float group0[ValueSumType::NGroup][ValueSumType::NGroup][FeatureDim];
    valueSumToFloat(layer0, valueSum.global, boardSizeScale);
    for (int i = 0; i < ValueSumType::NGroup; i++)
        for (int j = 0; j < ValueSumType::NGroup; j++)
            valueSumToFloat(group0[i][j], valueSum.group[i][j], groupSizeScale[i][j]);

    // group linear layer
    alignas(Alignment) float group1[ValueSumType::NGroup][ValueSumType::NGroup][ValueGroupDim];
    simd::linearLayer<simd::Activation::None>(group1[0][0],
                                              group0[0][0],
                                              bucket.value_corner_weight,
                                              bucket.value_corner_bias);
    simd::linearLayer<simd::Activation::None>(group1[0][2],
                                              group0[0][2],
                                              bucket.value_corner_weight,
                                              bucket.value_corner_bias);
    simd::linearLayer<simd::Activation::None>(group1[2][0],
                                              group0[2][0],
                                              bucket.value_corner_weight,
                                              bucket.value_corner_bias);
    simd::linearLayer<simd::Activation::None>(group1[2][2],
                                              group0[2][2],
                                              bucket.value_corner_weight,
                                              bucket.value_corner_bias);
    simd::linearLayer<simd::Activation::None>(group1[0][1],
                                              group0[0][1],
                                              bucket.value_edge_weight,
                                              bucket.value_edge_bias);
    simd::linearLayer<simd::Activation::None>(group1[1][0],
                                              group0[1][0],
                                              bucket.value_edge_weight,
                                              bucket.value_edge_bias);
    simd::linearLayer<simd::Activation::None>(group1[1][2],
                                              group0[1][2],
                                              bucket.value_edge_weight,
                                              bucket.value_edge_bias);
    simd::linearLayer<simd::Activation::None>(group1[2][1],
                                              group0[2][1],
                                              bucket.value_edge_weight,
                                              bucket.value_edge_bias);
    simd::linearLayer<simd::Activation::None>(group1[1][1],
                                              group0[1][1],
                                              bucket.value_center_weight,
                                              bucket.value_center_bias);
    simd::preluLayer(group1[0][0], group1[0][0], bucket.value_corner_prelu);
    simd::preluLayer(group1[0][2], group1[0][2], bucket.value_corner_prelu);
    simd::preluLayer(group1[2][0], group1[2][0], bucket.value_corner_prelu);
    simd::preluLayer(group1[2][2], group1[2][2], bucket.value_corner_prelu);
    simd::preluLayer(group1[0][1], group1[0][1], bucket.value_edge_prelu);
    simd::preluLayer(group1[1][0], group1[1][0], bucket.value_edge_prelu);
    simd::preluLayer(group1[1][2], group1[1][2], bucket.value_edge_prelu);
    simd::preluLayer(group1[2][1], group1[2][1], bucket.value_edge_prelu);
    simd::preluLayer(group1[1][1], group1[1][1], bucket.value_center_prelu);

    // quadrant linear layer
    alignas(Alignment) float quad0[2][2][ValueGroupDim];
    alignas(Alignment) float quad1[2][2][ValueGroupDim];
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
            simd::copy<ValueGroupDim>(quad0[i][j], group1[i + 0][j + 0]);
            simd::add<ValueGroupDim>(quad0[i][j], quad0[i][j], group1[i + 0][j + 1]);
            simd::add<ValueGroupDim>(quad0[i][j], quad0[i][j], group1[i + 1][j + 0]);
            simd::add<ValueGroupDim>(quad0[i][j], quad0[i][j], group1[i + 1][j + 1]);
            simd::linearLayer<simd::Activation::None>(quad1[i][j],
                                                      quad0[i][j],
                                                      bucket.value_quad_weight,
                                                      bucket.value_quad_bias);
            simd::preluLayer(quad1[i][j], quad1[i][j], bucket.value_quad_prelu);
        }
    simd::copy<ValueGroupDim>(layer0 + FeatureDim + 0 * ValueGroupDim, quad1[0][0]);
    simd::copy<ValueGroupDim>(layer0 + FeatureDim + 1 * ValueGroupDim, quad1[0][1]);
    simd::copy<ValueGroupDim>(layer0 + FeatureDim + 2 * ValueGroupDim, quad1[1][0]);
    simd::copy<ValueGroupDim>(layer0 + FeatureDim + 3 * ValueGroupDim, quad1[1][1]);

    // linear 1
    alignas(Alignment) float layer1[ValueDim];
    simd::linearLayer<simd::Activation::Relu>(layer1,
                                              layer0,
                                              bucket.value_l1_weight,
                                              bucket.value_l1_bias);

    // linear 2
    alignas(Alignment) float layer2[ValueDim];
    simd::linearLayer<simd::Activation::Relu>(layer2,
                                              layer1,
                                              bucket.value_l2_weight,
                                              bucket.value_l2_bias);

    // linear 3 final
    alignas(Alignment) float value[16];
    simd::linearLayer<simd::Activation::None>(value,
                                              layer2,
                                              bucket.value_l3_weight,
                                              bucket.value_l3_bias);

    return {value[0], value[1], value[2]};
}

void Mix8Accumulator::evaluatePolicy(const Mix8Weight &w, PolicyBuffer &policyBuffer)
{
    const auto &bucket = w.buckets[getBucketIndex()];

    typedef Batch<FeatureDim, float>       VSumB;
    typedef Batch<FeatureDWConvDim, float> ConvB;

    // convert global value sum from int32 to float
    alignas(Alignment) float globalValueMean[FeatureDim];

    auto scaleConv   = F32Op::set1(boardSizeScale * w.value_sum_scale_after_conv);
    auto scaleDirect = F32Op::set1(boardSizeScale * w.value_sum_scale_direct);
    for (int b = 0; b < ConvB::NumBatch; b++) {
        auto valueI32 = I32LS::load(valueSum.global.data() + b * VSumB::RegWidth);
        auto valueF32 = Convert<int32_t, float>::convert1(valueI32);
        valueF32      = F32Op::mul(valueF32, scaleConv);
        F32LS::store(globalValueMean + b * VSumB::RegWidth, valueF32);
    }
    for (int b = ConvB::NumBatch; b < VSumB::NumBatch; b++) {
        auto valueI32 = I32LS::load(valueSum.global.data() + b * VSumB::RegWidth);
        auto valueF32 = Convert<int32_t, float>::convert1(valueI32);
        valueF32      = F32Op::mul(valueF32, scaleDirect);
        F32LS::store(globalValueMean + b * VSumB::RegWidth, valueF32);
    }

    // policy pwconv weight layer
    alignas(Alignment) float pwconvWeight1[PolicyDim];
    simd::linearLayer<simd::Activation::None>(pwconvWeight1,
                                              globalValueMean,
                                              bucket.policy_pwconv_layer_l1_weight,
                                              bucket.policy_pwconv_layer_l1_bias);
    simd::preluLayer(pwconvWeight1, pwconvWeight1, bucket.policy_pwconv_layer_l1_prelu);

    alignas(Alignment) float pwconvWeight2[4 * PolicyDim];
    simd::linearLayer<simd::Activation::None>(pwconvWeight2,
                                              pwconvWeight1,
                                              bucket.policy_pwconv_layer_l2_weight,
                                              bucket.policy_pwconv_layer_l2_bias);

    int boardSizeSub1 = boardSize - 1;
    for (int y = 0, innerIdx = 0, outerIdx = fullBoardSize + 1; y < boardSize; y++, outerIdx += 2) {
        for (int x = 0; x < boardSize; x++, innerIdx++, outerIdx++) {
            if (!policyBuffer.getComputeFlag(innerIdx))
                continue;

            typedef Batch<PolicyDim, int16_t> PolicyB;
            typedef Batch<PolicyDim, float>   PWConvB;
            static_assert(PolicyDim <= FeatureDWConvDim,
                          "Assume PolicyDim <= FeatureDWConvDim in evaluatePolicy()!");

            // Apply relu, convert to float and accumulate all channels of pwconv feature
            float policy[4] = {0.0f};
            for (int b = 0; b < PolicyB::NumBatch; b++) {
                // Apply relu to dwconv feature sum
                auto policyI16Feat =
                    I16LS::load(mapAfterDWConv[outerIdx].data() + b * PolicyB::RegWidth);
                policyI16Feat = I16Op::max(policyI16Feat, I16Op::setzero());

                // Convert policy feature from int16 to float
                auto [policyI32Feat0, policyI32Feat1] =
                    Convert<int16_t, int32_t>::convert(policyI16Feat);
                auto policyFeat0 = Convert<int32_t, float>::convert1(policyI32Feat0);
                auto policyFeat1 = Convert<int32_t, float>::convert1(policyI32Feat1);

                // Apply pwconv by accumulating all channels of pwconv feature
                const int offset0 = (b * 2) * PWConvB::RegWidth;
                const int offset1 = (b * 2 + 1) * PWConvB::RegWidth;
                for (int i = 0; i < 4; i++) {
                    auto convW0   = F32LS::load(pwconvWeight2 + i * PolicyDim + offset0);
                    auto convW1   = F32LS::load(pwconvWeight2 + i * PolicyDim + offset1);
                    auto convSum0 = F32Op::mul(convW0, policyFeat0);
                    auto convSum1 = F32Op::mul(convW1, policyFeat1);
                    auto convSum  = F32Op::add(convSum0, convSum1);
                    policy[i] += F32Op::reduceadd(convSum);
                }
            }

            // Apply policy output PReLU and linear
            for (int i = 0; i < 4; i++) {
                float w   = policy[i] < 0 ? bucket.policy_output_neg_weight[i]
                                          : bucket.policy_output_pos_weight[i];
                policy[i] = policy[i] * w;
            }
            policyBuffer(innerIdx) =
                policy[0] + policy[1] + policy[2] + policy[3] + bucket.policy_output_bias;
        }
    }
}

Mix8Evaluator::Mix8Evaluator(int                   boardSize,
                             Rule                  rule,
                             std::filesystem::path blackWeightPath,
                             std::filesystem::path whiteWeightPath)
    : Evaluator(boardSize, rule)
    , weight {nullptr, nullptr}
{
    CompressedWrapper<StandardHeaderParserWarpper<Mix8BinaryWeightLoader>> loader(
        Compressor::Type::LZ4_DEFAULT);

    std::filesystem::path currentWeightPath;
    loader.setHeaderValidator([&](StandardHeader header) -> bool {
        constexpr uint32_t ArchHash =
            ArchHashBase
            ^ (((FeatureDWConvDim / 8) << 26) | ((ValueGroupDim / 8) << 20) | ((ValueDim / 8) << 14)
               | ((PolicyDim / 8) << 8) | (FeatureDim / 8));
        if (header.archHash != ArchHash)
            throw IncompatibleWeightFileError("incompatible architecture in weight file.");

        if (!contains(header.supportedRules, rule))
            throw UnsupportedRuleError(rule);

        if (!contains(header.supportedBoardSizes, boardSize))
            throw UnsupportedBoardSizeError(boardSize);

        if (Config::MessageMode != MsgMode::NONE)
            MESSAGEL("mix8nnue: load weight from " << currentWeightPath);
        return true;
    });

    for (const auto &[weightSide, weightPath] : {
             std::make_pair(BLACK, blackWeightPath),
             std::make_pair(WHITE, whiteWeightPath),
         }) {
        currentWeightPath  = weightPath;
        weight[weightSide] = Mix8WeightRegistry.loadWeightFromFile(weightPath, loader);
        if (!weight[weightSide])
            throw std::runtime_error("failed to load nnue weight from " + weightPath.string());
    }

    accumulator[BLACK] = std::make_unique<Mix8Accumulator>(boardSize);
    accumulator[WHITE] = std::make_unique<Mix8Accumulator>(boardSize);

    int numCells = boardSize * boardSize;
    moveCache[BLACK].reserve(numCells);
    moveCache[WHITE].reserve(numCells);

    valueSumBoardHistory[BLACK].reserve(numCells);
    valueSumBoardHistory[WHITE].reserve(numCells);
}

Mix8Evaluator::~Mix8Evaluator()
{
    if (weight[BLACK])
        Mix8WeightRegistry.unloadWeight(weight[BLACK]);
    if (weight[WHITE])
        Mix8WeightRegistry.unloadWeight(weight[WHITE]);
}

void Mix8Evaluator::initEmptyBoard()
{
    moveCache[BLACK].clear();
    moveCache[WHITE].clear();
    accumulator[BLACK]->clear(*weight[BLACK]);
    accumulator[WHITE]->clear(*weight[WHITE]);
}

void Mix8Evaluator::beforeMove(const Board &board, Pos pos)
{
    addCache(board.sideToMove(), pos.x(), pos.y(), false);
}

void Mix8Evaluator::afterUndo(const Board &board, Pos pos)
{
    addCache(board.sideToMove(), pos.x(), pos.y(), true);
}

ValueType Mix8Evaluator::evaluateValue(const Board &board)
{
    Color self = board.sideToMove(), oppo = ~self;

    // Apply all incremental update for both sides and calculate value
    clearCache(self);
    auto [win, loss, draw] = accumulator[self]->evaluateValue(*weight[self]);

    return ValueType(win, loss, draw, true);
}

void Mix8Evaluator::evaluatePolicy(const Board &board, PolicyBuffer &policyBuffer)
{
    Color self = board.sideToMove();

    // Apply all incremental update and calculate policy
    clearCache(self);
    accumulator[self]->evaluatePolicy(*weight[self], policyBuffer);
}

void Mix8Evaluator::clearCache(Color side)
{
    constexpr Color opponentMap[4] = {WHITE, BLACK, WALL, EMPTY};

    for (MoveCache &mc : moveCache[side]) {
        if (side == WHITE) {
            mc.oldColor = opponentMap[mc.oldColor];
            mc.newColor = opponentMap[mc.newColor];
        }

        if (mc.oldColor == EMPTY) {
            valueSumBoardHistory[side].push_back(accumulator[side]->valueSum);
            accumulator[side]->update<Mix8Accumulator::MOVE>(*weight[side],
                                                             mc.newColor,
                                                             mc.x,
                                                             mc.y,
                                                             nullptr);
        }
        else {
            accumulator[side]->update<Mix8Accumulator::UNDO>(*weight[side],
                                                             mc.oldColor,
                                                             mc.x,
                                                             mc.y,
                                                             &valueSumBoardHistory[side].back());
            valueSumBoardHistory[side].pop_back();
        }
    }
    moveCache[side].clear();
}

void Mix8Evaluator::addCache(Color side, int x, int y, bool isUndo)
{
    Color oldColor = EMPTY;
    Color newColor = side;
    if (isUndo)
        std::swap(oldColor, newColor);

    MoveCache newCache {oldColor, newColor, (int8_t)x, (int8_t)y};

    for (Color c : {BLACK, WHITE}) {
        if (moveCache[c].empty() || !isContraryMove(newCache, moveCache[c].back()))
            moveCache[c].push_back(newCache);
        else
            moveCache[c].pop_back();  // cancel out the last move cache

        assert(moveCache[c].size() < boardSize * boardSize);
    }
}

}  // namespace Evaluation::mix8
