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
#include "account.h"

#include <algorithm>
#include <boost/numeric/conversion/cast.hpp>
#include <boost/variant/apply_visitor.hpp>

#include "common/error.h"
#include "common/expect.h"
#include "light_wallet_server/db/data.h"
#include "light_wallet_server/db/string.h"

namespace lws
{
    struct account::internal
    {
        explicit internal(db::account const& source)
          : address(db::address_string(source.address)), id(source.id), pubs(source.address), view_key()
        {
            using inner_type =
                std::remove_reference<decltype(tools::unwrap(view_key))>::type;

            static_assert(
                std::is_standard_layout<db::view_key>(), "need standard layout source"
            );
            static_assert(std::is_pod<inner_type>(), "need pod target");
            static_assert(sizeof(view_key) == sizeof(source.key), "different size keys");
            std::memcpy(
                std::addressof(tools::unwrap(view_key)),
                std::addressof(source.key),
                sizeof(source.key)
            );
        }

        std::string address;
        db::account_id id;
        db::account_address pubs;
        crypto::secret_key view_key;
    };

    account::account(std::shared_ptr<const internal> immutable, db::block_id height, std::vector<db::output_id> received_) noexcept
      : immutable(std::move(immutable))
      , received_(std::move(received_))
      , spends_()
      , outputs_()
      , height(height)
    {}

    void account::null_check() const
    {
        if (!immutable)
            MONERO_THROW(::common_error::kInvalidArgument, "using moved from account");
    }

    account::account(db::account const& source, std::vector<db::output_id> received_)
      : account(std::make_shared<internal>(source), source.scan_height, std::move(received_))
    {
        std::sort(received_.begin(), received_.end());
    }

    account::~account() noexcept
    {}

    account account::clone() const
    {
        account result{immutable, height, received_};
        result.outputs_ = outputs_;
        result.spends_ = spends_;
        return result;
    }

    void account::updated(db::block_id new_height) noexcept
    {
        height = new_height;
        spends_.clear();
        spends_.shrink_to_fit();
        outputs_.clear();
        outputs_.shrink_to_fit();
    }

    db::account_id account::id() const noexcept
    {
        if (immutable)
            return immutable->id;
        return db::account_id::kInvalid;
    }

    std::string const& account::address() const
    {
        null_check();
        return immutable->address;
    }

    db::account_address const& account::db_address() const
    {
        null_check();
        return immutable->pubs;
    }

    crypto::public_key const& account::view_public() const
    {
        null_check();
        return immutable->pubs.view_public;
    }

    crypto::public_key const& account::spend_public() const
    {
        null_check();
        return immutable->pubs.spend_public;
    }

    crypto::secret_key const& account::view_key() const
    {
        null_check();
        return immutable->view_key;
    }

    void account::add_out(db::output const& out)
    {
        outputs_.push_back(out);
        received_.insert(
            std::lower_bound(received_.begin(), received_.end(), out.id),
            out.id
        );
    }

    void account::check_spends(crypto::key_image const& image, epee::span<const std::uint64_t> new_spends)
    {
        const std::uint32_t mixin = boost::numeric_cast<std::uint32_t>(
            std::max(std::size_t(1), new_spends.size()) - 1
        );

        std::uint64_t id = 0;
        for (std::uint64_t offset : new_spends)
        {
            id += offset;
            if (std::binary_search(received_.begin(), received_.end(), db::output_id(id)))
                spends_.emplace_back(db::output_id(id), db::spend{image, mixin});
        }
    }
} // lws
