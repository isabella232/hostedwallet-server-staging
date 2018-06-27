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
#include "scanner.h"

#include <algorithm>
#include <boost/numeric/conversion/cast.hpp>
#include <boost/range/combine.hpp>
#include <boost/thread/condition_variable.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/thread.hpp>
#include <cassert>
#include <chrono>
#include <cstring>
#include <type_traits>
#include <utility>
#include <zmq.h>

#include "common/error.h"
#include "crypto/crypto.h"
#include "cryptonote_basic/cryptonote_basic.h"
#include "cryptonote_basic/cryptonote_format_utils.h"
#include "cryptonote_core/cryptonote_tx_utils.h"
#include "light_wallet_server/account.h"
#include "light_wallet_server/db/data.h"
#include "light_wallet_server/error.h"
#include "misc_log_ex.h"
#include "rpc/daemon_messages.h"
#include "rpc/message.h"

#undef MONERO_DEFAULT_LOG_CATEGORY
#define MONERO_DEFAULT_LOG_CATEGORY "lws"

namespace
{
    constexpr const char stop_scan_endpoint[] = "inproc://stop_scan";

    constexpr const std::chrono::seconds account_poll_interval{10};
    constexpr const std::chrono::seconds block_poll_interval{20};
    constexpr const std::chrono::minutes block_rpc_timeout{2};
    constexpr const std::chrono::seconds send_timeout{30};
    constexpr const std::chrono::seconds sync_rpc_timeout{30};
}

namespace mzmq
{
namespace
{ 
    enum class error : int {};

    inline std::error_code make_error_code(mzmq::error value) noexcept
    {
        struct category final : std::error_category
        {
            virtual const char* name() const noexcept override final
            {
                return "mzmq::error_category()";
            }

            virtual std::string message(int value) const override final
            {
                char const* const msg = zmq_strerror(value);
                if (msg)
                    return msg;
                return "zmq_strerror failure";
            }
        };
        static const category instance{};
        return std::error_code{int(value), instance};
    }
}
}

namespace std
{
    template<>
    struct is_error_code_enum<::mzmq::error>
      : true_type
    {};
}

namespace mzmq
{
namespace
{
    struct terminate
    {
        void operator()(void* ptr) const noexcept
        {
            if (ptr)
            {
                while (zmq_close(ptr))
                {
                    if (zmq_errno() != EINTR)
                        break;
                }
            }
        }
    };
    using context = std::unique_ptr<void, terminate>;

    struct close
    {
        void operator()(void* ptr) const noexcept
        {
            if (ptr)
                zmq_close(ptr);
        }  
    };
    using socket = std::unique_ptr<void, close>;


    struct sockets 
    {
        socket daemon;
        socket parent;

        static socket make_daemon_conn(void* const ctx, std::string const& daemon_addr)
        {
            mzmq::socket daemon{zmq_socket(ctx, ZMQ_REQ)};
            if (daemon == nullptr)
                MONERO_THROW(mzmq::error(zmq_errno()), "ZMQ socket initialization failure");
            if (zmq_connect(daemon.get(), daemon_addr.c_str()))
                MONERO_THROW(mzmq::error(zmq_errno()), "ZMQ connect failure");
            return daemon;
        }

        static sockets make(void* const ctx, std::string const& daemon_addr)
        {
            sockets out{make_daemon_conn(ctx, daemon_addr), nullptr};
            out.parent.reset(zmq_socket(ctx, ZMQ_SUB));
            if (out.parent == nullptr)
                MONERO_THROW(mzmq::error(zmq_errno()), "ZMQ socket initialization failure");
            if (zmq_setsockopt(out.parent.get(), ZMQ_SUBSCRIBE, "", 0))
                MONERO_THROW(mzmq::error(zmq_errno()), "ZMQ subscription failure");

            if (zmq_connect(out.parent.get(), stop_scan_endpoint))
                MONERO_THROW(mzmq::error(zmq_errno()), "ZMQ connect failure"); 
            return out;
        }
    };

    template<typename M>
    std::string make_message(char const* const name, M& message)
    {
        return cryptonote::rpc::FullMessage::requestMessage(name, std::addressof(message)).getJson();
    }

    template<typename M>
    expect<M> unpack_message(zmq_msg_t& zmsg)
    {
        using namespace cryptonote::rpc;

        char const* const json = reinterpret_cast<const char*>(zmq_msg_data(std::addressof(zmsg)));
        if (json == nullptr)
            return {common_error::kInvalidArgument};

        M msg{};
        msg.fromJson(
            FullMessage{std::string{json, zmq_msg_size(std::addressof(zmsg))}}.getMessage()
        );
        return msg;
    }

    expect<void> wait(sockets& comm, short events, std::chrono::milliseconds timeout)
    {
        zmq_pollitem_t items[2]{
            {comm.daemon.get(), -1, short(events | ZMQ_POLLERR), 0},
            {comm.parent.get(), -1, short(ZMQ_POLLIN | ZMQ_POLLERR), 0}
        };

        for (;;)
        {
            const auto start = std::chrono::steady_clock::now();
            const int ready = zmq_poll(items, 2, timeout.count());
            const auto end = std::chrono::steady_clock::now();
            const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(start - end);
            timeout -= std::min(spent, timeout);

            if (ready == 0)
                return {lws::error::kDaemonConnectionFailure};
            if (0 < ready)
                break;
            const int err = zmq_errno(); 
            if (err != EINTR)
                return {mzmq::error(err)};
        }
        if (items[1].revents)
            return {lws::error::kAbortScan};
        return success();
    }

    bool send(sockets& comm, std::string const& message)
    {
        std::chrono::milliseconds timeout = send_timeout;
        while (zmq_send(comm.daemon.get(), message.data(), message.size(), ZMQ_DONTWAIT) < 0)
        {
            const int err = zmq_errno();
            if (err == EINTR)
                continue;
            if (err != EAGAIN)
                MONERO_THROW(mzmq::error(err), "ZMQ send failure");

            const expect<void> ready = wait(comm, ZMQ_POLLOUT, send_timeout);
            if (!ready)
            {
                if (ready == lws::error::kAbortScan)
                    return false;
                MONERO_THROW(ready.error(), "ZMQ send failure");
            } 
            timeout = std::chrono::seconds{0};
        }
        return true;
    }

    template<typename M>
    expect<M> receive(sockets& comm, std::chrono::milliseconds timeout)
    {
        zmq_msg_t zmsg;
        if (zmq_msg_init(std::addressof(zmsg)))
            return {mzmq::error(zmq_errno())};

        while (zmq_recvmsg(comm.daemon.get(), std::addressof(zmsg), ZMQ_DONTWAIT) < 0)
        {
            int err = zmq_errno();
            if (err == EINTR)
                continue;
            if (err != EAGAIN)
                return {mzmq::error(err)};

            const expect<void> ready = wait(comm, ZMQ_POLLIN, timeout);
            if (!ready)
                return ready.error();
            timeout = std::chrono::seconds{0};
        }
        return unpack_message<M>(zmsg);
    }
} // anonymous
} // mzmq

namespace lws
{
    std::atomic<bool> scanner::running{true};

    namespace
    {
        struct thread_sync
        {
            boost::mutex sync;
            boost::condition_variable user_poll;
            std::atomic<bool> update;
        };

        struct thread_data
        {
            explicit thread_data(mzmq::sockets comm, db::storage disk, std::vector<lws::account> users)
              : comm(std::move(comm)), disk(std::move(disk)), users(std::move(users))
            {}

            mzmq::sockets comm;
            db::storage disk;
            std::vector<lws::account> users;
        };

        // until we have a signal-handler safe notification system
        void checked_wait(const std::chrono::milliseconds wait)
        {
            const auto sleep_time = std::min(wait, std::chrono::milliseconds{500});
            const auto start = std::chrono::steady_clock::now();
            while (scanner::is_running() && (std::chrono::steady_clock::now() - start) < wait)
                boost::this_thread::sleep_for(boost::chrono::milliseconds{sleep_time.count()});
        }

        struct by_height
        {
            bool operator()(account const& left, account const& right) const noexcept
            {
                return left.scan_height() < right.scan_height();
            }
        };

        //! delays creation of temporary string in case DEBUG messages are being skipped
        struct money
        {
            std::uint64_t amount;
        };
        std::ostream& operator<<(std::ostream& out, money const src)
        {
           return out << cryptonote::print_money(src.amount);
        }


        void scan_transaction(
            epee::span<lws::account> users,
            const db::block_id height,
            const std::uint64_t timestamp,
            boost::optional<crypto::hash> tx_hash,
            cryptonote::transaction const& tx,
            std::vector<std::uint64_t> const& out_ids)
        {
            if (2 < tx.version)
                throw std::runtime_error{"Unsupported tx version"};

            std::vector<cryptonote::tx_extra_field> extra;
            cryptonote::tx_extra_pub_key key;

            boost::optional<crypto::hash> prefix_hash;
            boost::optional<std::pair<std::uint8_t, db::output::payment_id_>> payment_id; 

            cryptonote::parse_tx_extra(tx.extra, extra);
            // allow partial parsing of tx extra (similar to wallet2.cpp)

            if (!cryptonote::find_tx_extra_field_by_type(extra, key))
                return;

            for (account& user : users)
            {
                if (height <= user.scan_height())
                    continue; // to next user

                crypto::key_derivation derived;
                if (!crypto::generate_key_derivation(key.pub_key, user.view_key(), derived))
                    throw std::runtime_error{"Key derivation failed"};

                std::size_t ring_size = 0;
                for (auto const& in : tx.vin)
                {
                    cryptonote::txin_to_key const* const in_data =
                        boost::get<cryptonote::txin_to_key>(std::addressof(in));
                    if (in_data)
                    {
                        ring_size = in_data->key_offsets.size();
                        user.check_spends(in_data->k_image, epee::to_span(in_data->key_offsets));
                    }
                }

                db::extra ext = (ring_size == 0) ?
                    db::extra::kCoinbase : db::extra::kNone;

                std::size_t index = -1;
                for (auto const& out : tx.vout)
                {
                    ++index;

                    cryptonote::txout_to_key const* const out_data =
                        boost::get<cryptonote::txout_to_key>(std::addressof(out.target));
                    if (!out_data)
                        continue; // to next output

                    crypto::public_key derived_pub;
                    const bool received =
                        crypto::derive_public_key(derived, index, user.spend_public(), derived_pub) &&
                        derived_pub == out_data->key;

                    if (!received)
                        continue; // to next output

                    if (!prefix_hash)
                    {
                        prefix_hash.emplace();
                        cryptonote::get_transaction_prefix_hash(tx, *prefix_hash);
                    }

                    if (!tx_hash)
                    {
                        tx_hash.emplace();
                        if (!cryptonote::get_transaction_hash(tx, *tx_hash))
                        {
                            MWARNING("Failed to get transaction hash, skipping tx");
                            continue; // to next output
                        }
                    }

                    std::uint64_t amount = out.amount;
                    rct::key mask{};
                    if (!amount)
                    {
                        const auto decrypted = cryptonote::decode_amount(
                            tx.rct_signatures.outPk.at(index).mask, tx.rct_signatures.ecdhInfo.at(index), derived, index
                        );
                        if (!decrypted)
                        {
                            MWARNING(user.address() << " failed to decrypt amount for tx " << *tx_hash << ", skipping output");
                            continue; // to next output
                        }
                        amount = decrypted->first;
                        mask = decrypted->second;
                        ext = db::extra(lmdb::to_native(ext) | lmdb::to_native(db::extra::kRingct));
                    }

                    if (!payment_id)
                    {
                        using namespace cryptonote;

                        payment_id.emplace();
                        tx_extra_nonce extra_nonce;
                        if (find_tx_extra_field_by_type(extra, extra_nonce))
                        {
                            if (get_payment_id_from_tx_extra_nonce(extra_nonce.nonce, payment_id->second.long_))
                                payment_id->first = sizeof(crypto::hash);
                            else if (get_encrypted_payment_id_from_tx_extra_nonce(extra_nonce.nonce, payment_id->second.short_))
                                payment_id->first = sizeof(crypto::hash8);
                        }
                    }

                    db::output::payment_id_ user_pid = payment_id->second;

                    MDEBUG("Found match for " << user.address() << " on tx " << *tx_hash << " for " << money{amount} << " XMR");
                    user.add_out(
                        db::output{
                            height,
                            db::output_id(out_ids.at(index)),
                            amount,
                            timestamp,
                            tx.unlock_time,
                            boost::numeric_cast<std::uint32_t>(std::max(std::size_t(1), ring_size) - 1),
                            boost::numeric_cast<std::uint32_t>(index),
                            *tx_hash,
                            *prefix_hash,
                            key.pub_key,
                            mask,
                            0, 0, 0, 0, 0, 0, 0, // reserved bytes
                            db::pack(ext, payment_id->first),
                            user_pid
                        }
                    );
                } // for all tx outs
            } // for all users
        }

        void scan_loop(thread_sync& self, std::shared_ptr<thread_data> data) noexcept
        {
            using rpc_command = cryptonote::rpc::GetBlocksFast;
            
            try
            {
                // boost::thread doesn't support move-only types + attributes
                mzmq::sockets comm{std::move(data->comm)};
                db::storage disk{std::move(data->disk)};
                std::vector<lws::account> users{std::move(data->users)};

                assert(!users.empty());
                assert(std::is_sorted(users.begin(), users.end(), by_height{}));

                data.reset();

                struct stop_
                {
                    thread_sync& self;
                    ~stop_() noexcept
                    {
                        self.update = true;
                        self.user_poll.notify_one();
                    }
                } stop{self};

                // RPC server assumes that `start_height == 0` means use
                // block ids. This technically skips genesis block.
                rpc_command::Request req{};
                req.start_height = std::uint64_t(users.begin()->scan_height());
                req.start_height = std::max(std::uint64_t(1), req.start_height);
                req.prune = false;

                std::string block_request = mzmq::make_message(rpc_command::name, req);
                if (!mzmq::send(comm, block_request))
                    return;

                std::vector<crypto::hash> blockchain{};

                while (!self.update && scanner::is_running())
                {
                    blockchain.clear();

                    auto resp = mzmq::receive<rpc_command::Response>(comm, block_rpc_timeout);
                    if (!resp)
                    {
                        if (resp == lws::error::kAbortScan)
                            return;
                        if (resp == lws::error::kDaemonConnectionFailure)
                        {
                            MWARNING("Block retrieval timeout, retrying");
                            if (!mzmq::send(comm, block_request))
                                return;
                            continue;
                        }
                        MONERO_THROW(resp.error(), "Failed to retrieve blocks from daemon");
                    }

                    if (resp->blocks.empty())
                        throw std::runtime_error{"Daemon unexpectedly returned zero blocks"};

                    if (resp->start_height != req.start_height)
                    {
                        MWARNING("Daemon sent wrong blocks, resetting state");
                        return;
                    }

                    // retrieve next blocks in background
                    req.start_height = resp->start_height + resp->blocks.size() - 1;
                    block_request = mzmq::make_message(rpc_command::name, req);
                    if (!mzmq::send(comm, block_request))
                        return;

                    if (resp->blocks.size() <= 1)
                    {
                        // ... how about some ZMQ push stuff? we can only dream ...
                        if (mzmq::wait(comm, 0, block_poll_interval) == lws::error::kAbortScan)
                            return;
                        continue;
                    }

                    if (resp->blocks.size() != resp->output_indices.size())
                        throw std::runtime_error{"Bad daemon response - need same number of blocks and indices"};

                    blockchain.push_back(cryptonote::get_block_hash(resp->blocks.front().block));

                    auto blocks = epee::to_span(resp->blocks);
                    auto indices = epee::to_span(resp->output_indices);

                    if (resp->start_height != 1)
                    {
                        // skip overlap block
                        blocks.remove_prefix(1);
                        indices.remove_prefix(1);
                    }
                    else
                        resp->start_height = 0;

                    for (auto block_data : boost::combine(blocks, indices))
                    {
                        ++(resp->start_height);

                        cryptonote::block const& block = boost::get<0>(block_data).block;
                        auto const& txes = boost::get<0>(block_data).transactions;

                        if (block.tx_hashes.size() != txes.size())
                            throw std::runtime_error{"Bad daemon response - need same number of txes and tx hashes"};

                        auto indices = epee::to_span(boost::get<1>(block_data));
                        if (indices.empty())
                            throw std::runtime_error{"Bad daemon response - missing coinbase tx indices"};

                        scan_transaction(
                            epee::to_mut_span(users),
                            db::block_id(resp->start_height),
                            block.timestamp,
                            boost::none,
                            block.miner_tx,
                            *(indices.begin())
                        );

                        indices.remove_prefix(1);
                        if (txes.size() != indices.size())
                            throw std::runtime_error{"Bad daemon respnse - need same number of txes and indices"};

                        for (auto tx_data : boost::combine(block.tx_hashes, txes, indices))
                        {
                            scan_transaction(
                                epee::to_mut_span(users),
                                db::block_id(resp->start_height),
                                block.timestamp,
                                boost::get<0>(tx_data),
                                boost::get<1>(tx_data),
                                boost::get<2>(tx_data)
                            );
                        }

                        blockchain.push_back(cryptonote::get_block_hash(block));
                    }

                    expect<std::size_t> updated = disk.update(
                        users.front().scan_height(), epee::to_span(blockchain), epee::to_span(users)
                    );
                    if (!updated)
                    {
                        if (updated == lws::error::kBlockchainReorg)
                        {
                            MINFO("Blockchain reorg detected, resetting state");
                            return;
                        }
                        MONERO_THROW(updated.error(), "Failed to update accounts on disk");
                    }

                    MINFO("Processed " << blocks.size() << " block(s) against " << users.size() << " account(s)");
                    if (*updated != users.size())
                    {
                        MWARNING("Only updated " << *updated << " account(s) out of " << users.size() << ", resetting");
                        return;
                    }

                    for (account& user : users)
                        user.updated(db::block_id(resp->start_height));
                }
            }
            catch (std::exception const& e)
            {
                scanner::stop();
                MERROR(e.what());
            }
            catch (...)
            {
                scanner::stop();
                MERROR("Unknown exception");
            }
        }

        /*!
            Launches `thread_count` threads to run `scan_loop`, and then polls
            for active account changes in background
        */
        void check_loop(db::storage const& disk, void* const ctx, std::string const& daemon_addr, std::size_t thread_count, std::vector<lws::account> users, std::vector<db::account_id> active)
        {
            assert(0 < thread_count);
            assert(0 < users.size());

            mzmq::socket pub{zmq_socket(ctx, ZMQ_PUB)};
            if (pub == nullptr)
                MONERO_THROW(mzmq::error(zmq_errno()), "Unable to create ZMQ PUB socket");
            if (zmq_bind(pub.get(), stop_scan_endpoint))
                MONERO_THROW(mzmq::error(zmq_errno()), "Unable to bind to ZMQ inproc");

            thread_sync self{};            
            std::vector<boost::thread> threads{};

            struct join_
            {
                thread_sync& self;
                std::vector<boost::thread>& threads;
                mzmq::socket pub;

                ~join_() noexcept
                {
                    self.update = true;
                    zmq_send(pub.get(), "", 0, 0);
                    for (auto& thread : threads)
                        thread.join();
                }
            } join{self, threads, std::move(pub)};

            /*
                The algorithm here is extremely basic. Users are divided evenly
                amongst the configurable thread count, and grouped by scan height.
                If an old account appears, some accounts (grouped on that thread)
                will be delayed in processing waiting for that account to catch up.
                Its not the greatest, but this "will have to do" for the first cut.
                Its not expected that many people will be running
                "enterprise level" of nodes where accounts are constantly added.

                Another "issue" is that each thread works independently instead of
                more cooperatively for scanning. This requires a bit more
                synchronization, so was left for later. Its likely worth doing to
                reduce the number of transfers from the daemon, and the bottleneck
                on the writes into LMDB.

                If the active user list changes, all threads are stopped/joined,
                and everything is re-started.
            */

            boost::thread::attributes attrs;
            attrs.set_stack_size(THREAD_STACK_SIZE);

            threads.reserve(thread_count);
            std::sort(users.begin(), users.end(), by_height{});

            MINFO("Starting scan loops on " << std::min(thread_count, users.size()) << " thread(s) with " << users.size() << " account(s)");

            const std::size_t per_thread = std::max(std::size_t(1), users.size() / thread_count);
            while (!users.empty() && --thread_count)
            {
                const std::size_t count = std::min(per_thread, users.size());
                std::vector<lws::account> thread_users{
                    std::make_move_iterator(users.end() - count), std::make_move_iterator(users.end())
                };
                users.erase(users.end() - count, users.end());
                auto data = std::make_shared<thread_data>(
                    mzmq::sockets::make(ctx, daemon_addr), disk.clone(), std::move(thread_users)
                );
                threads.emplace_back(attrs, std::bind(&scan_loop, std::ref(self), std::move(data)));
            }

            if (!users.empty())
            {
                auto data = std::make_shared<thread_data>(
                    mzmq::sockets::make(ctx, daemon_addr), disk.clone(), std::move(users)
                );
                threads.emplace_back(attrs, std::bind(&scan_loop, std::ref(self), std::move(data)));
            }

            auto last_check = std::chrono::steady_clock::now();

            lmdb::suspended_txn read_txn{};
            db::cursor::accounts accounts_cur{};
            boost::unique_lock<boost::mutex> lock{self.sync};

            while (scanner::is_running())
            {
                for (;;)
                {
                    // TODO use signalfd + ZMQ? Windows is the difficult case...
                    self.user_poll.wait_for(lock, boost::chrono::seconds{1});
                    if (self.update || !scanner::is_running())
                        return;
                    auto this_check = std::chrono::steady_clock::now();
                    if (account_poll_interval <= (this_check - last_check))
                    {
                        last_check = this_check;
                        break;
                    }
                }

                auto reader = disk.start_read(std::move(read_txn));
                if (!reader)
                {
                    if (reader.matches(std::errc::no_lock_available))
                    {
                        MWARNING("Failed to open DB read handle, retrying later");
                        continue;
                    }
                    MONERO_THROW(reader.error(), "Failed to open DB read handle");
                }

                auto current_users = MONERO_UNWRAP(
                    "Active user list",
                    reader->get_accounts(db::account_status::kActive, std::move(accounts_cur))
                );
                if (current_users.count() != active.size())
                {
                    MINFO("Change in active user accounts detected");
                    return;
                }

                for (auto user = current_users.make_iterator(); !user.is_end(); ++user)
                {
                    const db::account_id user_id = user.get_value<MONERO_FIELD(db::account, id)>();
                    if (!std::binary_search(active.begin(), active.end(), user_id))
                    {
                        MINFO("Change in active user accounts detected");
                        return;
                    }
                }

                read_txn = reader->finish_read();
                accounts_cur = current_users.give_cursor();
            }
        }

        expect<void> sync_chain(db::storage& disk, void* daemon)
        {
            using rpc_command = cryptonote::rpc::GetHashesFast;

            MINFO("Starting blockchain sync with daemon");

            rpc_command::Request req{};
            req.start_height = 0;
            {
                auto reader = disk.start_read();
                if (!reader)
                    return reader.error();

                auto chain = reader->get_chain_sync();
                if (!chain)
                    return chain.error();

                req.known_hashes = std::move(*chain);
            }

            for (;;)
            {
                if (req.known_hashes.empty())
                    return {lws::error::kBadBlockchain};

                const std::string msg = mzmq::make_message(rpc_command::name, req);
                auto start = std::chrono::steady_clock::now();
                while (zmq_send(daemon, msg.data(), msg.size(), ZMQ_DONTWAIT) < 0)
                {
                    if (!scanner::is_running())
                        return {lws::error::kAbortScan};

                    if (sync_rpc_timeout <= (std::chrono::steady_clock::now() - start))
                        return {lws::error::kDaemonConnectionFailure};

                    const int err = zmq_errno();
                    if (err == EINTR)
                        continue;
                    else if (err != EAGAIN)
                        return {mzmq::error(err)};

                    boost::this_thread::sleep_for(boost::chrono::seconds{1});
                }

                zmq_msg_t zmsg;
                if (zmq_msg_init(std::addressof(zmsg)))
                    return {mzmq::error(zmq_errno())};

                start = std::chrono::steady_clock::now();
                while (zmq_recvmsg(daemon, std::addressof(zmsg), ZMQ_DONTWAIT) < 0)
                {
                    if (!scanner::is_running())
                        return {lws::error::kAbortScan};

                    if (sync_rpc_timeout <= (std::chrono::steady_clock::now() - start))
                        return {lws::error::kDaemonConnectionFailure};

                    const int err = zmq_errno();
                    if (err == EINTR)
                        continue;
                    else if (err != EAGAIN)
                        return {mzmq::error(err)};

                    boost::this_thread::sleep_for(boost::chrono::seconds{1});
                }

                auto resp = mzmq::unpack_message<rpc_command::Response>(zmsg);
                if (!resp)
                    return resp.error();

                //
                // Exit loop if it appears we have synced to top of chain
                // 
                if (resp->hashes.size() <= 1 || resp->hashes.back() == req.known_hashes.front())
                    return success();

                MONERO_CHECK(disk.sync_chain(db::block_id(resp->start_height), resp->hashes));

                req.known_hashes.erase(req.known_hashes.begin(), --(req.known_hashes.end()));
                const auto loc = req.known_hashes.begin();
                for (std::size_t num = 0; num < 10; ++num)
                {
                    if (resp->hashes.empty())
                        break;
                    req.known_hashes.splice(
                        loc, resp->hashes, --(resp->hashes.end()), resp->hashes.end()
                    );
                }
            } 
        }
    } // anonymous
    
    scanner::scanner(db::storage disk_, std::string daemon_addr_)
      : disk(std::move(disk_)), daemon_addr(std::move(daemon_addr_))
    {
        mzmq::context ctx{zmq_init(1)};
        if (ctx == nullptr)
            MONERO_THROW(mzmq::error(zmq_errno()), "ZMQ context initialization failure");

        mzmq::socket daemon = mzmq::sockets::make_daemon_conn(ctx.get(), daemon_addr);
        assert(daemon != nullptr);
        MONERO_UNWRAP("Blockchain sync with daemon", sync_chain(disk, daemon.get()));
    }

    scanner::~scanner() noexcept
    {}

    void scanner::fetch_loop(std::size_t thread_count)
    {
        thread_count = std::max(std::size_t(1), thread_count);

        mzmq::context ctx{zmq_init(1)};
        if (ctx == nullptr)
            MONERO_THROW(mzmq::error(zmq_errno()), "ZMQ context initialization failure");

        mzmq::socket daemon = nullptr;

        for (;;)
        {
            std::vector<db::account_id> active;
            std::vector<lws::account> users;

            {
                MINFO("Retrieving current active account list");

                auto reader = MONERO_UNWRAP("Start DB read", disk.start_read());
                auto list = MONERO_UNWRAP(
                    "Active user list", reader.get_accounts(db::account_status::kActive)
                );

                for (auto current = list.make_iterator(); !current.is_end(); ++current)
                {
                    db::account user = *current;
                    std::vector<db::output_id> receives{};
                    auto receive_list = MONERO_UNWRAP(
                        "User receive list", reader.get_outputs(user.id)
                    );
                    auto id_range = receive_list.make_range<MONERO_FIELD(db::output, id)>();
                    std::copy(
                        id_range.begin(), id_range.end(), std::back_inserter(receives)
                    );
                    users.emplace_back(user, std::move(receives));
                    active.insert(
                        std::lower_bound(active.begin(), active.end(), user.id), user.id
                    );
                }

                reader.finish_read();
            }

            if (users.empty())
            {
                MINFO("No active accounts");
                checked_wait(account_poll_interval);
            }
            else
                check_loop(disk, ctx.get(), daemon_addr, thread_count, std::move(users), std::move(active));

            if (!scanner::is_running())
                return;

            if (!daemon)
                daemon = mzmq::sockets::make_daemon_conn(ctx.get(), daemon_addr);

            assert(daemon != nullptr);
            const expect<void> synced = sync_chain(disk, daemon.get());
            if (!synced)
            {
                if (!synced.matches(std::errc::connection_refused))
                    MONERO_THROW(synced.error(), "Unable to sync blockchain");

                MWARNING("Failed to connect to daemon at " << daemon_addr);
            }
        }
    }
} // lws