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

#include <cassert>
#include <cstdint>
#include <iosfwd>
#include <utility>

#include "crypto/crypto.h"
#include "lmdb/util.h"
#include "ringct/rctTypes.h" //! \TODO brings in lots of includes, try to remove

namespace lws
{
namespace db
{
    /*
        Enum classes are used because they generate identical code to native
        integer types, but are not implicitly convertible to each other or
        any integer types. They also have comparison but not arithmetic
        operators defined.
    */

    //! References an account stored in the database, faster than by address
    enum class account_id : std::uint32_t
    {
        kInvalid = std::uint32_t(-1) //!< Always represents _not an_ account id.
    };

    //! Number of seconds since UNIX epoch.
    enum class account_time : std::uint32_t {};

    //! References a block height
    enum class block_id : std::uint64_t {};

    //! References a global output number, as determined by the daemon
    enum class output_id : std::uint64_t {};

    enum class account_status : std::uint8_t
    {
        kActive = 0, //!< Actively being scanned and reported by API
        kInactive,   //!< Not being scanned, but still reported by API
        kHidden      //!< Not being scanned or reported by API
    };

    enum class request : std::uint8_t
    {
        kCreate = 0, //!< Add a new account
        kImportScan  //!< Set account start and scan height to zero.
    };

    /*!
        DB does not use `crypto::secret_key` because it is not POD
        (UB to copy over entire struct). LMDB is keeping a copy in process
        memory anyway (row encryption not currently used). The roadmap
        recommends process isolation per-connection by default as a defense
        against obtaining someone elses viewkey.
    */
    struct view_key : crypto::ec_scalar {};

    struct account_address
    {
        crypto::public_key spend_public;
        crypto::public_key view_public;
    };
    static_assert(sizeof(account_address) == 64, "padding in account_address");

    struct account
    {
        account_id id;          //!< Must be first for LMDB optimizations
        account_time access;    //!< Last time `get_address_info` was called.
        account_address address;
        view_key key;           //!< Doubles as authorization handle
        block_id scan_height;   //!< Last block scanned; check-ins are always by block
        block_id start_height;  //!< Account started scanning at this block height
        account_time creation;  //!< Time account first appeared in database.
        char reserved[4];
    };
    static_assert(sizeof(account) == (4 * 2) + 64 + 32 + (8 * 2) + (4 * 2), "padding in account");
    
    struct block_info
    {
        block_id id;      //!< Must be first for LMDB optimizations
        crypto::hash hash;
    };
    static_assert(sizeof(block_info) == 8 + 32, "padding in block_info");

    enum class extra : std::uint8_t
    {
        kNone = 0,
        kCoinbase = 1,
        kRingct = 2,
        kCoinbaseAndRingct = 3
    };

    enum class extra_and_length : std::uint8_t {};

    //! \return `val` and `length` packed into a single byte.
    inline extra_and_length pack(extra val, std::uint8_t length) noexcept
    {
        assert(length <= 32);
        return extra_and_length((std::uint8_t(val) & 0x7) | (length << 3));
    }

    //! \return `extra` and length unpacked from a single byte.
    inline std::pair<extra, std::uint8_t> unpack(extra_and_length val) noexcept
    {
        const std::uint8_t real_val = std::uint8_t(val);
        return {extra(real_val & 0x7), std::uint8_t(real_val >> 3)};
    }

    //! Information about an output that has been received by an `account`.
    struct output
    {
        block_id height;          //!< Must be first for LMDB optimizations
        output_id id;             //!< Must be second for LMDB optimizations
        std::uint64_t amount;
        std::uint64_t timestamp;
        std::uint64_t unlock_time;//!< Not always a timestamp; mirrors chain value.
        std::uint32_t mixin_count;//!< Ring-size of TX
        std::uint32_t index;      //!< Offset within a tx
        crypto::hash tx_hash;
        crypto::hash tx_prefix_hash;
        crypto::public_key tx_public;
        struct ringct_
        {
            rct::key mask;        //!< Unencrypted CT mask
        } ringct;
        char reserved[7];
        extra_and_length extra;   //!< Extra info + length of payment id
        union payment_id_
        {
            crypto::hash8 short_; //!< Decrypted short payment id
            crypto::hash long_;   //!< Long version of payment id (always decrypted)
        } payment_id;
    };
    static_assert(sizeof(output) == (8 * 5) + (4 * 2) + (32 * 4) + 7 + 1 + 32, "padding in output");

    //! Information about a possible spend of a received `output`.
    struct spend
    {
        crypto::key_image image;  //!< Must be first for LMDB optimizations
        std::uint32_t mixin_count;//!< Ring-size of TX spending output
    };
    static_assert(sizeof(spend) == 32 + 4, "padding in spend");

    struct request_info
    {
        account_address address;//!< Must be first for LMDB optimizations
        view_key key;
        block_id start_height;
        account_time creation;  //!< Time the request was created.
        char reserved[4];
    };
    static_assert(sizeof(request_info) == 64 + 32 + 8 + (4 * 2), "padding in request_info");

    /*!
        Write `address` to `out` in base58 format using
        `lws::config::network` to determine tag.
    */
    std::ostream& operator<<(std::ostream& out, account_address const& address);
} // db
} // lws
