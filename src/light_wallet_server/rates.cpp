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
#include "rates.h"

#include <rapidjson/document.h>

#include "light_wallet_server/json.h"
#include "serialization/new/json_error.h"

namespace lws
{
namespace rpc
{
    const char crypto_compare_::host[] = "https://min-api.cryptocompare.com:443";
    const char crypto_compare_::path[] =
        "/data/price?fsym=XMR&tsyms=AUD,BRL,BTC,CAD,CHF,CNY,EUR,GBP,"
        "HKD,INR,JPY,KRW,MXN,NOK,NZD,SEK,SGD,TRY,USD,RUB,ZAR";

    expect<lws::rates> crypto_compare_::operator()(std::string const& body) const noexcept
    {
        rapidjson::Document doc{};
        if (!rapidjson::ParseResult(doc.Parse(body.c_str())))
            return {::json::error::kParseFailure};

        lws::rates out{};
        MONERO_CHECK(json::rates(doc, out));
        return out;
    }
} // rpc
} // lws
