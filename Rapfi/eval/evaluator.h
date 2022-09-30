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

#pragma once

#include "../core/pos.h"
#include "../core/types.h"

#include <cassert>
#include <stdexcept>
#include <string>

class Board;

namespace Evaluation {

/// ValueType is a container for value (and a optional draw rate).
/// Draw rate value less than 0.0 means no draw rate is contained.
class ValueType
{
public:
    explicit ValueType(Value value) : val(value) {}
    explicit ValueType(float winLogits,
                       float lossLogits,
                       float drawLogits,
                       bool  applySoftmax = true);
    bool  hasWinLossRate() const { return winRate >= 0.0f && lossRate >= 0.0f; }
    bool  hasDrawRate() const { return drawRate >= 0.0f; }
    float win() const { return winRate; }
    float loss() const { return lossRate; }
    float draw() const { return drawRate; }
    float winLossRate() const { return winRate - lossRate; }
    Value value() const
    {
        assert(val != VALUE_NONE);
        return val;
    }

    /// Construct a new value from the given draw winning rate.
    /// @param drawWinRate The winning rate of draw result.
    /// @param drawRate The draw rate of new value. (default is 0)
    ///     This value should not be greater than current draw rate.
    ValueType valueOfDrawWinRate(float drawWinRate, float drawRate = 0.0f);

private:
    Value val      = VALUE_NONE;
    float winRate  = -1.0f;
    float lossRate = -1.0f;
    float drawRate = -1.0f;
};

/// PolicyBuffer is a container for float policy values on board.
class PolicyBuffer
{
public:
    using PolicyType                       = float;
    static constexpr PolicyType ScoreScale = 32;
    static constexpr Score      ScoreBias  = 300;

    PolicyBuffer(int boardSize) : PolicyBuffer(boardSize, boardSize) {}
    PolicyBuffer(int boardWidth, int boardHeight);

    PolicyType &operator[](Pos pos) { return policy[posToIndex(pos)]; }
    PolicyType  operator[](Pos pos) const { return policy[posToIndex(pos)]; }
    PolicyType &operator()(int x, int y) { return policy[boardWidth * y + x]; }
    PolicyType  operator()(int x, int y) const { return policy[boardWidth * y + x]; }
    PolicyType &operator()(int index) { return policy[index]; }
    PolicyType  operator()(int index) const { return policy[index]; }
    void  setComputeFlag(Pos pos, bool enabled = true) { computeFlag[posToIndex(pos)] = enabled; }
    void  setComputeFlagForAllEmptyCell(const Board &board, bool enabled = true);
    void  setComputeFlagForAllCandidateCell(const Board &board, bool enabled = true);
    bool  getComputeFlag(int x, int y) const { return computeFlag[boardWidth * y + x]; }
    bool  getComputeFlag(int index) const { return computeFlag[index]; }
    void  setScoreBias(Score bias) { policyScoreBias = ScoreBias + bias; }
    Score score(Pos pos) const { return Score((*this)[pos] * ScoreScale) + policyScoreBias; }

    /// Applies softmax to all computed policy.
    void applySoftmax();

private:
    inline size_t posToIndex(Pos pos) const
    {
        size_t index = boardWidth * pos.y() + pos.x();
        assert(0 <= pos.x() && pos.x() < boardWidth);
        assert(index < bufferSize);
        return index;
    }

    int        boardWidth;
    int        bufferSize;
    float      policyScoreBias;
    bool       computeFlag[MAX_MOVES];
    PolicyType policy[MAX_MOVES];
};

/// Evaluator is the base class for evaluation plugins.
/// It provides overridable hook over board move/undo update, and interface for doing value
/// evaluation and policy evaluation. Different evaluation implementation may inherit from
/// this class to replace the default classical evaluation builtin the board.
class Evaluator
{
public:
    /// Constructor sets the board size and rule of the evaluator.
    /// Default behaviour supports all size and rule. If the evaluator does
    /// not support this rule or this board size, it throws an exception.
    Evaluator(int boardSize, Rule rule);
    virtual ~Evaluator() = default;

    /// Resets the evaluator state to empty board.
    virtual void initEmptyBoard() = 0;
    /// Update hook called before board.move().
    virtual void beforeMove(const Board &board, Pos pos) {};
    /// Update hook called after board.move().
    virtual void afterMove(const Board &board, Pos pos) {};
    /// Update hook called before board.undo().
    virtual void beforeUndo(const Board &board, Pos pos) {};
    /// Update hook called after board.undo().
    virtual void afterUndo(const Board &board, Pos pos) {};

    /// @brief Sync evaluator state with the given board state.
    /// This is implemented as initEmptyBoard() as well as a sequence of beforeMove()
    /// and afterMove() by default.
    virtual void syncWithBoard(const Board &board);

    /// Evaluates value for current side to move.
    virtual ValueType evaluateValue(const Board &board) = 0;
    /// Evaluates policy for current side to move.
    virtual void evaluatePolicy(const Board &board, PolicyBuffer &policyBuffer) = 0;

    const int  boardSize;
    const Rule rule;
};

/// Helper base class for reporting unsupported evaluator config.
struct UnsupportedEvaluatorError : public ::std::runtime_error
{
    UnsupportedEvaluatorError(::std::string message) : ::std::runtime_error(message) {}
};

/// Helper class for reporting unsupported rule.
struct UnsupportedRuleError : public UnsupportedEvaluatorError
{
    const Rule rule;
    UnsupportedRuleError(Rule rule, ::std::string message = "")
        : UnsupportedEvaluatorError(message)
        , rule(rule)
    {}
};

/// Helper class for reporting unsupported board size.
struct UnsupportedBoardSizeError : public UnsupportedEvaluatorError
{
    const int boardSize;
    UnsupportedBoardSizeError(int boardSize, ::std::string message = "")
        : UnsupportedEvaluatorError(message)
        , boardSize(boardSize)
    {}
};

/// Helper class for reporting incompatible weight file (arch mismatch, etc).
struct IncompatibleWeightFileError : public UnsupportedEvaluatorError
{
    IncompatibleWeightFileError(::std::string message = "") : UnsupportedEvaluatorError(message) {}
};

}  // namespace Evaluation
