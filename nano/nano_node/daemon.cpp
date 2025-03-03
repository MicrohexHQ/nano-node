#include <nano/boost/process.hpp>
#include <nano/lib/utility.hpp>
#include <nano/nano_node/daemon.hpp>
#include <nano/node/daemonconfig.hpp>
#include <nano/node/ipc.hpp>
#include <nano/node/json_handler.hpp>
#include <nano/node/node.hpp>
#include <nano/node/openclwork.hpp>
#include <nano/rpc/rpc.hpp>
#include <nano/secure/working.hpp>

#include <boost/property_tree/json_parser.hpp>

#include <csignal>
#include <fstream>
#include <iostream>

namespace
{
void my_abort_signal_handler (int signum)
{
	std::signal (signum, SIG_DFL);
	nano::dump_crash_stacktrace ();
	nano::create_load_memory_address_files ();
}
}

namespace
{
volatile sig_atomic_t sig_int_or_term = 0;
}

void nano_daemon::daemon::run (boost::filesystem::path const & data_path, nano::node_flags const & flags)
{
	// Override segmentation fault and aborting.
	std::signal (SIGSEGV, &my_abort_signal_handler);
	std::signal (SIGABRT, &my_abort_signal_handler);

	boost::filesystem::create_directories (data_path);
	boost::system::error_code error_chmod;
	nano::set_secure_perm_directory (data_path, error_chmod);
	std::unique_ptr<nano::thread_runner> runner;
	nano::daemon_config config (data_path);
	auto error = nano::read_node_config_toml (data_path, config, flags.config_overrides);
	nano::set_use_memory_pools (config.node.use_memory_pools);
	if (!error)
	{
		config.node.logging.init (data_path);
		nano::logger_mt logger{ config.node.logging.min_time_between_log_output };
		boost::asio::io_context io_ctx;
		auto opencl (nano::opencl_work::create (config.opencl_enable, config.opencl, logger));
		nano::work_pool opencl_work (config.node.work_threads, config.node.pow_sleep_interval, opencl ? [&opencl](nano::uint256_union const & root_a, uint64_t difficulty_a, std::atomic<int> & ticket_a) {
			return opencl->generate_work (root_a, difficulty_a, ticket_a);
		}
		                                                                                              : std::function<boost::optional<uint64_t> (nano::uint256_union const &, uint64_t, std::atomic<int> &)> (nullptr));
		nano::alarm alarm (io_ctx);
		try
		{
			auto node (std::make_shared<nano::node> (io_ctx, data_path, alarm, config.node, opencl_work, flags));
			if (!node->init_error ())
			{
				auto network_label = node->network_params.network.get_current_network_as_string ();
				std::cout << "Network: " << network_label << ", version: " << NANO_VERSION_STRING << "\n"
				          << "Path: " << node->application_path.string () << "\n"
				          << "Build Info: " << BUILD_INFO << std::endl;

				node->start ();
				nano::ipc::ipc_server ipc_server (*node, config.rpc);
#if BOOST_PROCESS_SUPPORTED
				std::unique_ptr<boost::process::child> rpc_process;
#endif
				std::unique_ptr<std::thread> rpc_process_thread;
				std::unique_ptr<nano::rpc> rpc;
				std::unique_ptr<nano::rpc_handler_interface> rpc_handler;
				if (config.rpc_enable)
				{
					if (!config.rpc.child_process.enable)
					{
						// Launch rpc in-process
						nano::rpc_config rpc_config;
						auto error = nano::read_rpc_config_toml (data_path, rpc_config);
						if (error)
						{
							std::cout << error.get_message () << std::endl;
							std::exit (1);
						}
						rpc_handler = std::make_unique<nano::inprocess_rpc_handler> (*node, config.rpc, [&ipc_server, &alarm, &io_ctx]() {
							ipc_server.stop ();
							alarm.add (std::chrono::steady_clock::now () + std::chrono::seconds (3), [&io_ctx]() {
								io_ctx.stop ();
							});
						});
						rpc = nano::get_rpc (io_ctx, rpc_config, *rpc_handler);
						rpc->start ();
					}
					else
					{
						// Spawn a child rpc process
						if (!boost::filesystem::exists (config.rpc.child_process.rpc_path))
						{
							throw std::runtime_error (std::string ("RPC is configured to spawn a new process however the file cannot be found at: ") + config.rpc.child_process.rpc_path);
						}

						auto network = node->network_params.network.get_current_network_as_string ();
#if BOOST_PROCESS_SUPPORTED
						rpc_process = std::make_unique<boost::process::child> (config.rpc.child_process.rpc_path, "--daemon", "--data_path", data_path, "--network", network);
#else
						auto rpc_exe_command = boost::str (boost::format ("%1% --daemon --data_path=%2% --network=%3%") % config.rpc.child_process.rpc_path % data_path % network);
						// clang-format off
						rpc_process_thread = std::make_unique<std::thread> ([rpc_exe_command, &logger = node->logger]() {
							nano::thread_role::set (nano::thread_role::name::rpc_process_container);
							std::system (rpc_exe_command.c_str ());
							logger.always_log ("RPC server has stopped");
						});
						// clang-format on
#endif
					}
				}

				assert (!nano::signal_handler_impl);
				nano::signal_handler_impl = [&io_ctx]() {
					io_ctx.stop ();
					sig_int_or_term = 1;
				};

				std::signal (SIGINT, &nano::signal_handler);
				std::signal (SIGTERM, &nano::signal_handler);

				runner = std::make_unique<nano::thread_runner> (io_ctx, node->config.io_threads);
				runner->join ();

				if (sig_int_or_term == 1)
				{
					ipc_server.stop ();
					node->stop ();
					if (rpc)
					{
						rpc->stop ();
					}
				}
#if BOOST_PROCESS_SUPPORTED
				if (rpc_process)
				{
					rpc_process->wait ();
				}
#else
				if (rpc_process_thread)
				{
					rpc_process_thread->join ();
				}
#endif
			}
			else
			{
				std::cerr << "Error initializing node\n";
			}
		}
		catch (const std::runtime_error & e)
		{
			std::cerr << "Error while running node (" << e.what () << ")\n";
		}
	}
	else
	{
		std::cerr << "Error deserializing config: " << error.get_message () << std::endl;
	}
}
