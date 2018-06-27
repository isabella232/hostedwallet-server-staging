// Copyright (c) 2018, The Monero Project
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#pragma once

#include <system_error>
#include <type_traits>

namespace lws
{
    enum class error : int
    {
        // 0 is reserved for no error, as per expect<T>
        kAbortScan = 1,           //!< Thread needs to stop scanning
        kAccountExists,           //!< Tried to create an account that already exists
        kBadAddress,              //!< Invalid base58 public address
        kBadViewKey,              //!< Account has address/viewkey mismatch
        kBadBlockchain,           //!< Blockchain is invalid or wrong network type
        kBlockchainReorg,         //!< Blockchain reorg after fetching/scanning block(s)
        kCreateQueueMax,          //!< Reached maximum pending account requests
        kDaemonConnectionFailure, //!< ZMQ connection failure with daemon
        kDuplicateRequest,        //!< Account already has a request of  this type pending
        kExceededBlockchainBuffer,//!< Out buffer for blockchain is too small
        kNoSuchAccount,           //!< Account address is not in database.
        kSystemClockInvalidRange  //!< System clock is out of range for storage format
    };

    std::error_category const& error_category() noexcept;

    inline std::error_code make_error_code(lws::error value) noexcept
    {
        return std::error_code{int(value), error_category()};
    }
}

namespace std
{
    template<>
    struct is_error_code_enum<::lws::error>
      : true_type
    {};
}
