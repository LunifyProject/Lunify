// Copyright (c) 2014-2023, The Monero Project
// Copyright (c) 2021-2023, Haku Labs MTÜ
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
// this list of
//    conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
// this list
//    of conditions and the following disclaimer in the documentation and/or
//    other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors
// may be
//    used to endorse or promote products derived from this software without
//    specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Parts of this file are originally copyright (c) 2012-2013 The Cryptonote
// developers

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <boost/math/special_functions/round.hpp>

#include "int-util.h"
#include "crypto/hash.h"
#include "cryptonote_config.h"
#include "difficulty.h"

#undef lunify_DEFAULT_LOG_CATEGORY
#define lunify_DEFAULT_LOG_CATEGORY "difficulty"

namespace cryptonote {

using std::size_t;
using std::uint64_t;
using std::vector;

#if defined(__x86_64__)
static inline void mul(uint64_t a, uint64_t b, uint64_t &low, uint64_t &high) {
  low = mul128(a, b, &high);
}

#else

static inline void mul(uint64_t a, uint64_t b, uint64_t &low, uint64_t &high) {
  // __int128 isn't part of the standard, so the previous function wasn't
  // portable. mul128() in Windows is fine, but this portable function should be
  // used elsewhere. Credit for this function goes to latexi95.

  uint64_t aLow = a & 0xFFFFFFFF;
  uint64_t aHigh = a >> 32;
  uint64_t bLow = b & 0xFFFFFFFF;
  uint64_t bHigh = b >> 32;

  uint64_t res = aLow * bLow;
  uint64_t lowRes1 = res & 0xFFFFFFFF;
  uint64_t carry = res >> 32;

  res = aHigh * bLow + carry;
  uint64_t highResHigh1 = res >> 32;
  uint64_t highResLow1 = res & 0xFFFFFFFF;

  res = aLow * bHigh;
  uint64_t lowRes2 = res & 0xFFFFFFFF;
  carry = res >> 32;

  res = aHigh * bHigh + carry;
  uint64_t highResHigh2 = res >> 32;
  uint64_t highResLow2 = res & 0xFFFFFFFF;

  // Addition

  uint64_t r = highResLow1 + lowRes2;
  carry = r >> 32;
  low = (r << 32) | lowRes1;
  r = highResHigh1 + highResLow2 + carry;
  uint64_t d3 = r & 0xFFFFFFFF;
  carry = r >> 32;
  r = highResHigh2 + carry;
  high = d3 | (r << 32);
}

#endif

static inline bool cadd(uint64_t a, uint64_t b) { return a + b < a; }

static inline bool cadc(uint64_t a, uint64_t b, bool c) {
  return a + b < a || (c && a + b == (uint64_t)-1);
}

bool check_hash_64(const crypto::hash &hash, uint64_t difficulty) {
  uint64_t low, high, top, cur;
  // First check the highest word, this will most likely fail for a random hash.
  mul(swap64le(((const uint64_t *)&hash)[3]), difficulty, top, high);
  if (high != 0) {
    return false;
  }
  mul(swap64le(((const uint64_t *)&hash)[0]), difficulty, low, cur);
  mul(swap64le(((const uint64_t *)&hash)[1]), difficulty, low, high);
  bool carry = cadd(cur, low);
  cur = high;
  mul(swap64le(((const uint64_t *)&hash)[2]), difficulty, low, high);
  carry = cadc(cur, low, carry);
  carry = cadc(high, top, carry);
  return !carry;
}

uint64_t next_difficulty_64(std::vector<std::uint64_t> timestamps,
                            std::vector<uint64_t> cumulative_difficulties,
                            size_t target_seconds) {
  const int64_t T = static_cast<int64_t>(target_seconds);
  size_t N = DIFFICULTY_WINDOW_NEW;
  int64_t FTL = static_cast<int64_t>(CRYPTONOTE_BLOCK_FUTURE_TIME_LIMIT_NEW);

  // Return a difficulty of 1 for first 3 blocks if it's the start of the chain.
  if (timestamps.size() < 4) {
    return 1;
  }
  // Otherwise, use a smaller N if the start of the chain is less than N+1.
  else if (timestamps.size() < N + 1) {
    N = timestamps.size() - 1;
  }
  // Otherwise make sure timestamps and cumulative_difficulties are correct
  // size.
  else {
    timestamps.resize(N + 1);
    cumulative_difficulties.resize(N + 1);
  }
  // To get an average solvetime to within +/- ~0.1%, use an adjustment factor.
  // adjust=0.998 for N = 60
  const double adjust = 0.998;
  // The divisor k normalizes the LWMA sum to a standard LWMA.
  const double k = N * (N + 1) / 2;

  double LWMA(0), sum_inverse_D(0), harmonic_mean_D(0), nextDifficulty(0);
  int64_t solveTime(0);
  uint64_t difficulty(0), next_difficulty(0);

  // Loop through N most recent blocks. N is most recently solved block.
  for (size_t i = 1; i <= N; i++) {
    solveTime = static_cast<int64_t>(timestamps[i]) -
                static_cast<int64_t>(timestamps[i - 1]);
    solveTime = std::min<int64_t>((T * 10), std::max<int64_t>(solveTime, -FTL));
    difficulty = cumulative_difficulties[i] - cumulative_difficulties[i - 1];
    LWMA += (int64_t)(solveTime * i) / k;
    sum_inverse_D += 1 / static_cast<double>(difficulty);
  }

  harmonic_mean_D = N / sum_inverse_D;

  // Limit LWMA same as Bitcoin's 1/4 in case something unforeseen occurs.
  if (static_cast<int64_t>(boost::math::round(LWMA)) < T / 4)
    LWMA = static_cast<double>(T / 4);

  nextDifficulty = harmonic_mean_D * T / LWMA * adjust;

  // No limits should be employed, but this is correct way to employ a 20%
  // symmetrical limit:
  // nextDifficulty=max(previous_Difficulty*0.8,min(previous_Difficulty/0.8,
  // next_Difficulty));
  next_difficulty = static_cast<uint64_t>(nextDifficulty);
  return next_difficulty;
}

#if defined(_MSC_VER)
#ifdef max
#undef max
#endif
#endif

const difficulty_type max64bit(std::numeric_limits<std::uint64_t>::max());
const boost::multiprecision::uint256_t
    max128bit(std::numeric_limits<boost::multiprecision::uint128_t>::max());
const boost::multiprecision::uint512_t
    max256bit(std::numeric_limits<boost::multiprecision::uint256_t>::max());

#define FORCE_FULL_128_BITS

bool check_hash_128(const crypto::hash &hash, difficulty_type difficulty) {
#ifndef FORCE_FULL_128_BITS
  // fast check
  if (difficulty >= max64bit && ((const uint64_t *)&hash)[3] > 0)
    return false;
#endif
  // usual slow check
  boost::multiprecision::uint512_t hashVal = 0;
#ifdef FORCE_FULL_128_BITS
  for (int i = 0; i < 4; i++) { // highest word is zero
#else
  for (int i = 1; i < 4; i++) { // highest word is zero
#endif
    hashVal <<= 64;
    hashVal |= swap64le(((const uint64_t *)&hash)[3 - i]);
  }
  return hashVal * difficulty <= max256bit;
}

bool check_hash(const crypto::hash &hash, difficulty_type difficulty) {
  if (difficulty <= max64bit) // if can convert to small difficulty - do it
    return check_hash_64(hash, difficulty.convert_to<std::uint64_t>());
  else
    return check_hash_128(hash, difficulty);
}

difficulty_type
next_difficulty(std::vector<std::uint64_t> timestamps,
                std::vector<difficulty_type> cumulative_difficulties,
                size_t target_seconds) {
  const int64_t T = static_cast<int64_t>(target_seconds);
  size_t N = DIFFICULTY_WINDOW_NEW;
  int64_t FTL = static_cast<int64_t>(CRYPTONOTE_BLOCK_FUTURE_TIME_LIMIT_NEW);

  // Return a difficulty of 1 for first 3 blocks if it's the start of the chain.
  if (timestamps.size() < 4) {
    return 1;
  }
  // Otherwise, use a smaller N if the start of the chain is less than N+1.
  else if (timestamps.size() < N + 1) {
    N = timestamps.size() - 1;
  }
  // Otherwise make sure timestamps and cumulative_difficulties are correct
  // size.
  else {
    timestamps.resize(N + 1);
    cumulative_difficulties.resize(N + 1);
  }
  // To get an average solvetime to within +/- ~0.1%, use an adjustment factor.
  // adjust=0.998 for N = 60
  const double adjust = 0.998;
  // The divisor k normalizes the LWMA sum to a standard LWMA.
  const double k = N * (N + 1) / 2;

  double LWMA(0), sum_inverse_D(0), harmonic_mean_D(0), nextDifficulty(0);
  int64_t solveTime(0);
  difficulty_type difficulty(0), next_difficulty(0);

  // Loop through N most recent blocks. N is most recently solved block.
  for (size_t i = 1; i <= N; i++) {
    solveTime = static_cast<int64_t>(timestamps[i]) -
                static_cast<int64_t>(timestamps[i - 1]);
    solveTime = std::min<int64_t>((T * 10), std::max<int64_t>(solveTime, -FTL));
    difficulty = cumulative_difficulties[i] - cumulative_difficulties[i - 1];
    LWMA += (int64_t)(solveTime * i) / k;
    sum_inverse_D += 1 / difficulty.convert_to<double>();
  }

  harmonic_mean_D = N / sum_inverse_D;

  // Limit LWMA same as Bitcoin's 1/4 in case something unforeseen occurs.
  if (static_cast<int64_t>(boost::math::round(LWMA)) < T / 4)
    LWMA = static_cast<double>(T / 4);

  nextDifficulty = harmonic_mean_D * T / LWMA * adjust;

  // No limits should be employed, but this is correct way to employ a 20%
  // symmetrical limit:
  // nextDifficulty=max(previous_Difficulty*0.8,min(previous_Difficulty/0.8,
  // next_Difficulty));
  next_difficulty = static_cast<difficulty_type>(nextDifficulty);
  if (next_difficulty.is_zero())
    next_difficulty = 1;
  return next_difficulty;
}

std::string hex(difficulty_type v) {
  static const char chars[] = "0123456789abcdef";
  std::string s;
  while (v > 0) {
    s.push_back(chars[(v & 0xf).convert_to<unsigned>()]);
    v >>= 4;
  }
  if (s.empty())
    s += "0";
  std::reverse(s.begin(), s.end());
  return "0x" + s;
}
} // namespace cryptonote
