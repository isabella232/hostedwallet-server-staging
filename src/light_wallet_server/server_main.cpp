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

#include <boost/optional/optional.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/thread/thread.hpp>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

#include "common/command_line.h"
#include "common/expect.h"
#include "cryptonote_config.h"
#include "light_wallet_server/config.h"
#include "light_wallet_server/db/storage.h"
#include "light_wallet_server/options.h"
#include "light_wallet_server/rest_server.h"
#include "light_wallet_server/scanner.h"

#undef MONERO_DEFAULT_LOG_CATEGORY
#define MONERO_DEFAULT_LOG_CATEGORY "lws"

namespace
{
    struct options : lws::options
    {
        const command_line::arg_descriptor<std::string> daemon_rpc;
        const command_line::arg_descriptor<std::string> rest_server;
        const command_line::arg_descriptor<std::size_t> rest_threads;
        const command_line::arg_descriptor<std::size_t> scan_threads;
        const command_line::arg_descriptor<bool> external_bind;
        const command_line::arg_descriptor<unsigned> create_queue_max;
        const command_line::arg_descriptor<std::chrono::minutes::rep> rates_interval;

        static std::string get_default_zmq()
        {
            static constexpr const char base[] = "tcp://127.0.0.1:";
            switch (lws::config::network)
            {
                case cryptonote::TESTNET:
                    return base + std::to_string(config::testnet::ZMQ_RPC_DEFAULT_PORT);
                case cryptonote::STAGENET:
                    return base + std::to_string(config::stagenet::ZMQ_RPC_DEFAULT_PORT);
                case cryptonote::MAINNET:
                default:
                    break;
            }
            return base + std::to_string(config::ZMQ_RPC_DEFAULT_PORT);
        }

        options()
          : lws::options()
          , daemon_rpc{"daemon", "<protocol>://<address>:<port> of a monerod ZMQ RPC", get_default_zmq()}
          , rest_server{"rest-server", "[address:]<port> for incoming connections", "http://127.0.0.1:8080"}
          , rest_threads{"rest-threads", "Number of threads to process REST conections", 1}
          , scan_threads{"scan-threads", "Maximum number of threads for account scanning", boost::thread::hardware_concurrency()}
          , external_bind{"external-bind", "Allow listening for external connections"}
          , create_queue_max{"create-queue-max", "Set pending create account requests maximum", 10000}
          , rates_interval{"exchange-rate-interval", "Retrieve exchange rates in minute intervals from cryptocompare.com if greater than 0", 0}
        {}

        void prepare(boost::program_options::options_description& description) const
        {
            lws::options::prepare(description);
            command_line::add_arg(description, daemon_rpc);
            command_line::add_arg(description, rest_server);
            command_line::add_arg(description, rest_threads);
            command_line::add_arg(description, scan_threads);
            command_line::add_arg(description, external_bind);
            command_line::add_arg(description, create_queue_max);
            command_line::add_arg(description, rates_interval);
        }
    };

    struct program
    {
        std::string db_path;
        std::string rest_server;
        std::string daemon_rpc;
        std::chrono::minutes rates_interval;
        std::size_t rest_threads;
        std::size_t scan_threads;
        unsigned create_queue_max;
    };

    void print_help(std::ostream& out)
    {
        boost::program_options::options_description description{"Options"};
        options{}.prepare(description);

        out << "Usage: [options]" << std::endl;
        out << description;
    }

    boost::optional<program> get_program(int argc, char** argv)
    {
        namespace po = boost::program_options;

        const options opts{};
        po::variables_map args{};
        {
            po::options_description description{"Options"};
            opts.prepare(description);

            po::store(
                po::command_line_parser(argc, argv).options(description).run(), args
            );
            po::notify(args);
        }

        if (command_line::get_arg(args, command_line::arg_help))
        {
            print_help(std::cout);
            return boost::none;
        }

        opts.set_network(args); // do this first, sets global variable :/
        program prog{
            command_line::get_arg(args, opts.db_path),
            command_line::get_arg(args, opts.rest_server),
            command_line::get_arg(args, opts.daemon_rpc),
            std::chrono::minutes{command_line::get_arg(args, opts.rates_interval)},
            command_line::get_arg(args, opts.rest_threads),
            command_line::get_arg(args, opts.scan_threads),
            command_line::get_arg(args, opts.create_queue_max)
        };

        prog.rest_threads = std::max(std::size_t(1), prog.rest_threads);
        prog.scan_threads = std::max(std::size_t(1), prog.scan_threads);

        if (command_line::is_arg_defaulted(args, opts.daemon_rpc))
            prog.daemon_rpc = options::get_default_zmq();

        return prog;
    }

    void run(program prog)
    {
        std::signal(SIGINT, [] (int) { lws::scanner::stop(); });

        auto disk = lws::db::storage::open(prog.db_path.c_str(), prog.create_queue_max);
        auto ctx = lws::rpc::context::make(std::move(prog.daemon_rpc), prog.rates_interval);

        MINFO("Using monerod ZMQ RPC at " << ctx.daemon_address());
        auto client = lws::scanner::sync(disk.clone(), ctx.connect().value()).value();

        lws::rest_server server{disk.clone(), std::move(client)};
        MONERO_UNWRAP(
            "REST server start", server.run(prog.rest_server, prog.rest_threads)
        );
        MINFO("Listening for REST clients at " << prog.rest_server);

        // blocks until SIGINT
        lws::scanner::run(std::move(disk), std::move(ctx), prog.scan_threads);
    }
}

int main(int argc, char** argv)
{
    try
    {
        boost::optional<program> prog;

        try
        { 
            prog = get_program(argc, argv);
        }
        catch (std::exception const& e)
        {
            std::cerr << e.what() << std::endl << std::endl;
            print_help(std::cerr);
            return EXIT_FAILURE;
        }

        if (prog)
            run(std::move(*prog));
    }
    catch (std::exception const& e)
    {
        MERROR(e.what());
        return EXIT_FAILURE;
    }
    catch (...)
    {
        MERROR("Unknown exception");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}