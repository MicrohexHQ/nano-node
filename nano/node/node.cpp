#include <nano/crypto_lib/random_pool.hpp>
#include <nano/lib/timer.hpp>
#include <nano/lib/utility.hpp>
#include <nano/node/common.hpp>
#include <nano/node/node.hpp>
#include <nano/rpc/rpc.hpp>

#if NANO_ROCKSDB
#include <nano/node/rocksdb/rocksdb.hpp>
#endif

#include <boost/polymorphic_cast.hpp>
#include <boost/property_tree/json_parser.hpp>

#include <algorithm>
#include <cstdlib>
#include <future>
#include <numeric>
#include <sstream>

double constexpr nano::node::price_max;
double constexpr nano::node::free_cutoff;
size_t constexpr nano::block_arrival::arrival_size_min;
std::chrono::seconds constexpr nano::block_arrival::arrival_time_min;

namespace nano
{
extern unsigned char nano_bootstrap_weights_live[];
extern size_t nano_bootstrap_weights_live_size;
extern unsigned char nano_bootstrap_weights_beta[];
extern size_t nano_bootstrap_weights_beta_size;
}

void nano::node::keepalive (std::string const & address_a, uint16_t port_a)
{
	auto node_l (shared_from_this ());
	network.resolver.async_resolve (boost::asio::ip::udp::resolver::query (address_a, std::to_string (port_a)), [node_l, address_a, port_a](boost::system::error_code const & ec, boost::asio::ip::udp::resolver::iterator i_a) {
		if (!ec)
		{
			for (auto i (i_a), n (boost::asio::ip::udp::resolver::iterator{}); i != n; ++i)
			{
				auto endpoint (nano::transport::map_endpoint_to_v6 (i->endpoint ()));
				std::weak_ptr<nano::node> node_w (node_l);
				auto channel (node_l->network.find_channel (endpoint));
				if (!channel)
				{
					node_l->network.tcp_channels.start_tcp (endpoint, [node_w](std::shared_ptr<nano::transport::channel> channel_a) {
						if (auto node_l = node_w.lock ())
						{
							node_l->network.send_keepalive (channel_a);
						}
					});
				}
				else
				{
					node_l->network.send_keepalive (channel);
				}
			}
		}
		else
		{
			node_l->logger.try_log (boost::str (boost::format ("Error resolving address: %1%:%2%: %3%") % address_a % port_a % ec.message ()));
		}
	});
}

namespace nano
{
std::unique_ptr<seq_con_info_component> collect_seq_con_info (rep_crawler & rep_crawler, const std::string & name)
{
	size_t count = 0;
	{
		std::lock_guard<std::mutex> guard (rep_crawler.active_mutex);
		count = rep_crawler.active.size ();
	}

	auto sizeof_element = sizeof (decltype (rep_crawler.active)::value_type);
	auto composite = std::make_unique<seq_con_info_composite> (name);
	composite->add_component (std::make_unique<seq_con_info_leaf> (seq_con_info{ "active", count, sizeof_element }));
	return composite;
}

std::unique_ptr<seq_con_info_component> collect_seq_con_info (block_processor & block_processor, const std::string & name)
{
	size_t state_blocks_count = 0;
	size_t blocks_count = 0;
	size_t blocks_hashes_count = 0;
	size_t forced_count = 0;
	size_t rolled_back_count = 0;

	{
		std::lock_guard<std::mutex> guard (block_processor.mutex);
		state_blocks_count = block_processor.state_blocks.size ();
		blocks_count = block_processor.blocks.size ();
		blocks_hashes_count = block_processor.blocks_hashes.size ();
		forced_count = block_processor.forced.size ();
		rolled_back_count = block_processor.rolled_back.size ();
	}

	auto composite = std::make_unique<seq_con_info_composite> (name);
	composite->add_component (std::make_unique<seq_con_info_leaf> (seq_con_info{ "state_blocks", state_blocks_count, sizeof (decltype (block_processor.state_blocks)::value_type) }));
	composite->add_component (std::make_unique<seq_con_info_leaf> (seq_con_info{ "blocks", blocks_count, sizeof (decltype (block_processor.blocks)::value_type) }));
	composite->add_component (std::make_unique<seq_con_info_leaf> (seq_con_info{ "blocks_hashes", blocks_hashes_count, sizeof (decltype (block_processor.blocks_hashes)::value_type) }));
	composite->add_component (std::make_unique<seq_con_info_leaf> (seq_con_info{ "forced", forced_count, sizeof (decltype (block_processor.forced)::value_type) }));
	composite->add_component (std::make_unique<seq_con_info_leaf> (seq_con_info{ "rolled_back", rolled_back_count, sizeof (decltype (block_processor.rolled_back)::value_type) }));
	composite->add_component (collect_seq_con_info (block_processor.generator, "generator"));
	return composite;
}
}

nano::node::node (boost::asio::io_context & io_ctx_a, uint16_t peering_port_a, boost::filesystem::path const & application_path_a, nano::alarm & alarm_a, nano::logging const & logging_a, nano::work_pool & work_a, nano::node_flags flags_a) :
node (io_ctx_a, application_path_a, alarm_a, nano::node_config (peering_port_a, logging_a), work_a, flags_a)
{
}

nano::node::node (boost::asio::io_context & io_ctx_a, boost::filesystem::path const & application_path_a, nano::alarm & alarm_a, nano::node_config const & config_a, nano::work_pool & work_a, nano::node_flags flags_a) :
io_ctx (io_ctx_a),
node_initialized_latch (1),
config (config_a),
stats (config.stat_config),
flags (flags_a),
alarm (alarm_a),
work (work_a),
logger (config_a.logging.min_time_between_log_output),
store_impl (nano::make_store (logger, application_path_a, flags.read_only, true, config_a.diagnostics_config.txn_tracking, config_a.block_processor_batch_max_time, config_a.lmdb_max_dbs, !flags.disable_unchecked_drop, flags.sideband_batch_size, config_a.backup_before_upgrade)),
store (*store_impl),
wallets_store_impl (std::make_unique<nano::mdb_wallets_store> (application_path_a / "wallets.ldb", config_a.lmdb_max_dbs)),
wallets_store (*wallets_store_impl),
gap_cache (*this),
ledger (store, stats, config.epoch_block_link, config.epoch_block_signer, flags_a.cache_representative_weights_from_frontiers),
checker (config.signature_checker_threads),
network (*this, config.peering_port),
bootstrap_initiator (*this),
bootstrap (config.peering_port, *this),
application_path (application_path_a),
port_mapping (*this),
vote_processor (*this),
rep_crawler (*this),
warmed_up (0),
block_processor (*this, write_database_queue),
block_processor_thread ([this]() {
	nano::thread_role::set (nano::thread_role::name::block_processing);
	this->block_processor.process_blocks ();
}),
online_reps (*this, config.online_weight_minimum.number ()),
vote_uniquer (block_uniquer),
active (*this),
confirmation_height_processor (pending_confirmation_height, store, ledger.stats, active, ledger.epoch_link, write_database_queue, config.conf_height_processor_batch_min_time, logger),
payment_observer_processor (observers.blocks),
wallets (wallets_store.init_error (), *this),
startup_time (std::chrono::steady_clock::now ())
{
	if (!init_error ())
	{
		if (config.websocket_config.enabled)
		{
			auto endpoint_l (nano::tcp_endpoint (config.websocket_config.address, config.websocket_config.port));
			websocket_server = std::make_shared<nano::websocket::listener> (*this, endpoint_l);
			this->websocket_server->run ();
		}

		wallets.observer = [this](bool active) {
			observers.wallet.notify (active);
		};
		network.channel_observer = [this](std::shared_ptr<nano::transport::channel> channel_a) {
			observers.endpoint.notify (channel_a);
		};
		network.disconnect_observer = [this]() {
			observers.disconnect.notify ();
		};
		if (!config.callback_address.empty ())
		{
			observers.blocks.add ([this](nano::election_status const & status_a, nano::account const & account_a, nano::amount const & amount_a, bool is_state_send_a) {
				auto block_a (status_a.winner);
				if ((status_a.type == nano::election_status_type::active_confirmed_quorum || status_a.type == nano::election_status_type::active_confirmation_height) && this->block_arrival.recent (block_a->hash ()))
				{
					auto node_l (shared_from_this ());
					background ([node_l, block_a, account_a, amount_a, is_state_send_a]() {
						boost::property_tree::ptree event;
						event.add ("account", account_a.to_account ());
						event.add ("hash", block_a->hash ().to_string ());
						std::string block_text;
						block_a->serialize_json (block_text);
						event.add ("block", block_text);
						event.add ("amount", amount_a.to_string_dec ());
						if (is_state_send_a)
						{
							event.add ("is_send", is_state_send_a);
							event.add ("subtype", "send");
						}
						// Subtype field
						else if (block_a->type () == nano::block_type::state)
						{
							if (block_a->link ().is_zero ())
							{
								event.add ("subtype", "change");
							}
							else if (amount_a == 0 && !node_l->ledger.epoch_link.is_zero () && node_l->ledger.is_epoch_link (block_a->link ()))
							{
								event.add ("subtype", "epoch");
							}
							else
							{
								event.add ("subtype", "receive");
							}
						}
						std::stringstream ostream;
						boost::property_tree::write_json (ostream, event);
						ostream.flush ();
						auto body (std::make_shared<std::string> (ostream.str ()));
						auto address (node_l->config.callback_address);
						auto port (node_l->config.callback_port);
						auto target (std::make_shared<std::string> (node_l->config.callback_target));
						auto resolver (std::make_shared<boost::asio::ip::tcp::resolver> (node_l->io_ctx));
						resolver->async_resolve (boost::asio::ip::tcp::resolver::query (address, std::to_string (port)), [node_l, address, port, target, body, resolver](boost::system::error_code const & ec, boost::asio::ip::tcp::resolver::iterator i_a) {
							if (!ec)
							{
								node_l->do_rpc_callback (i_a, address, port, target, body, resolver);
							}
							else
							{
								if (node_l->config.logging.callback_logging ())
								{
									node_l->logger.always_log (boost::str (boost::format ("Error resolving callback: %1%:%2%: %3%") % address % port % ec.message ()));
								}
								node_l->stats.inc (nano::stat::type::error, nano::stat::detail::http_callback, nano::stat::dir::out);
							}
						});
					});
				}
			});
		}
		if (websocket_server)
		{
			observers.blocks.add ([this](nano::election_status const & status_a, nano::account const & account_a, nano::amount const & amount_a, bool is_state_send_a) {
				assert (status_a.type != nano::election_status_type::ongoing);

				if (this->websocket_server->any_subscriber (nano::websocket::topic::confirmation))
				{
					auto block_a (status_a.winner);
					std::string subtype;
					if (is_state_send_a)
					{
						subtype = "send";
					}
					else if (block_a->type () == nano::block_type::state)
					{
						if (block_a->link ().is_zero ())
						{
							subtype = "change";
						}
						else if (amount_a == 0 && !this->ledger.epoch_link.is_zero () && this->ledger.is_epoch_link (block_a->link ()))
						{
							subtype = "epoch";
						}
						else
						{
							subtype = "receive";
						}
					}

					this->websocket_server->broadcast_confirmation (block_a, account_a, amount_a, subtype, status_a);
				}
			});

			observers.active_stopped.add ([this](nano::block_hash const & hash_a) {
				if (this->websocket_server->any_subscriber (nano::websocket::topic::stopped_election))
				{
					nano::websocket::message_builder builder;
					this->websocket_server->broadcast (builder.stopped_election (hash_a));
				}
			});

			observers.difficulty.add ([this](uint64_t active_difficulty) {
				if (this->websocket_server->any_subscriber (nano::websocket::topic::active_difficulty))
				{
					nano::websocket::message_builder builder;
					auto msg (builder.difficulty_changed (network_params.network.publish_threshold, active_difficulty));
					this->websocket_server->broadcast (msg);
				}
			});
		}
		// Add block confirmation type stats regardless of http-callback and websocket subscriptions
		observers.blocks.add ([this](nano::election_status const & status_a, nano::account const & account_a, nano::amount const & amount_a, bool is_state_send_a) {
			assert (status_a.type != nano::election_status_type::ongoing);
			switch (status_a.type)
			{
				case nano::election_status_type::active_confirmed_quorum:
					this->stats.inc (nano::stat::type::observer, nano::stat::detail::observer_confirmation_active_quorum, nano::stat::dir::out);
					break;
				case nano::election_status_type::active_confirmation_height:
					this->stats.inc (nano::stat::type::observer, nano::stat::detail::observer_confirmation_active_conf_height, nano::stat::dir::out);
					break;
				case nano::election_status_type::inactive_confirmation_height:
					this->stats.inc (nano::stat::type::observer, nano::stat::detail::observer_confirmation_inactive, nano::stat::dir::out);
					break;
				default:
					break;
			}
		});
		observers.endpoint.add ([this](std::shared_ptr<nano::transport::channel> channel_a) {
			if (channel_a->get_type () == nano::transport::transport_type::udp)
			{
				this->network.send_keepalive (channel_a);
			}
			else
			{
				this->network.send_keepalive_self (channel_a);
			}
		});
		observers.vote.add ([this](nano::transaction const & transaction, std::shared_ptr<nano::vote> vote_a, std::shared_ptr<nano::transport::channel> channel_a) {
			this->gap_cache.vote (vote_a);
			this->online_reps.observe (vote_a->account);
			nano::uint128_t rep_weight;
			{
				rep_weight = ledger.weight (transaction, vote_a->account);
			}
			if (rep_weight > minimum_principal_weight ())
			{
				bool rep_crawler_exists (false);
				for (auto hash : *vote_a)
				{
					if (this->rep_crawler.exists (hash))
					{
						rep_crawler_exists = true;
						break;
					}
				}
				if (rep_crawler_exists)
				{
					// We see a valid non-replay vote for a block we requested, this node is probably a representative
					if (this->rep_crawler.response (channel_a, vote_a->account, rep_weight))
					{
						logger.try_log (boost::str (boost::format ("Found a representative at %1%") % channel_a->to_string ()));
						// Rebroadcasting all active votes to new representative
						auto blocks (this->active.list_blocks (true));
						for (auto i (blocks.begin ()), n (blocks.end ()); i != n; ++i)
						{
							if (*i != nullptr)
							{
								nano::confirm_req req (*i);
								channel_a->send (req);
							}
						}
					}
				}
			}
		});
		if (this->websocket_server)
		{
			observers.vote.add ([this](nano::transaction const & transaction, std::shared_ptr<nano::vote> vote_a, std::shared_ptr<nano::transport::channel> channel_a) {
				if (this->websocket_server->any_subscriber (nano::websocket::topic::vote))
				{
					nano::websocket::message_builder builder;
					auto msg (builder.vote_received (vote_a));
					this->websocket_server->broadcast (msg);
				}
			});
		}

		logger.always_log ("Node starting, version: ", NANO_VERSION_STRING);
		logger.always_log ("Build information: ", BUILD_INFO);

		auto network_label = network_params.network.get_current_network_as_string ();
		logger.always_log ("Active network: ", network_label);

		logger.always_log (boost::str (boost::format ("Work pool running %1% threads %2%") % work.threads.size () % (work.opencl ? "(1 for OpenCL)" : "")));
		logger.always_log (boost::str (boost::format ("%1% work peers configured") % config.work_peers.size ()));
		if (config.work_peers.empty () && config.work_threads == 0 && !work.opencl)
		{
			logger.always_log ("Work generation is disabled");
		}

		if (config.logging.node_lifetime_tracing ())
		{
			logger.always_log ("Constructing node");
		}

		logger.always_log (boost::str (boost::format ("Outbound Voting Bandwidth limited to %1% bytes per second") % config.bandwidth_limit));

		// First do a pass with a read to see if any writing needs doing, this saves needing to open a write lock (and potentially blocking)
		auto is_initialized (false);
		{
			auto transaction (store.tx_begin_read ());
			is_initialized = (store.latest_begin (transaction) != store.latest_end ());
		}

		nano::genesis genesis;
		if (!is_initialized)
		{
			release_assert (!flags.read_only);
			auto transaction (store.tx_begin_write ());
			// Store was empty meaning we just created it, add the genesis block
			store.initialize (transaction, genesis, ledger.rep_weights);
		}

		auto transaction (store.tx_begin_read ());
		if (!store.block_exists (transaction, genesis.hash ()))
		{
			std::stringstream ss;
			ss << "Genesis block not found. Make sure the node network ID is correct.";
			if (network_params.network.is_beta_network ())
			{
				ss << " Beta network may have reset, try clearing database files";
			}
			auto str = ss.str ();

			logger.always_log (str);
			std::cerr << str << std::endl;
			std::exit (1);
		}

		node_id = nano::keypair ();
		logger.always_log ("Node ID: ", node_id.pub.to_node_id ());

		const uint8_t * weight_buffer = network_params.network.is_live_network () ? nano_bootstrap_weights_live : nano_bootstrap_weights_beta;
		size_t weight_size = network_params.network.is_live_network () ? nano_bootstrap_weights_live_size : nano_bootstrap_weights_beta_size;
		if (network_params.network.is_live_network () || network_params.network.is_beta_network ())
		{
			nano::bufferstream weight_stream ((const uint8_t *)weight_buffer, weight_size);
			nano::uint128_union block_height;
			if (!nano::try_read (weight_stream, block_height))
			{
				auto max_blocks = (uint64_t)block_height.number ();
				auto transaction (store.tx_begin_read ());
				if (ledger.store.block_count (transaction).sum () < max_blocks)
				{
					ledger.bootstrap_weight_max_blocks = max_blocks;
					while (true)
					{
						nano::account account;
						if (nano::try_read (weight_stream, account.bytes))
						{
							break;
						}
						nano::amount weight;
						if (nano::try_read (weight_stream, weight.bytes))
						{
							break;
						}
						logger.always_log ("Using bootstrap rep weight: ", account.to_account (), " -> ", weight.format_balance (Mxrb_ratio, 0, true), " XRB");
						ledger.bootstrap_weights[account] = weight.number ();
					}
				}
			}
		}
	}
	node_initialized_latch.count_down ();
}

nano::node::~node ()
{
	if (config.logging.node_lifetime_tracing ())
	{
		logger.always_log ("Destructing node");
	}
	stop ();
}

void nano::node::do_rpc_callback (boost::asio::ip::tcp::resolver::iterator i_a, std::string const & address, uint16_t port, std::shared_ptr<std::string> target, std::shared_ptr<std::string> body, std::shared_ptr<boost::asio::ip::tcp::resolver> resolver)
{
	if (i_a != boost::asio::ip::tcp::resolver::iterator{})
	{
		auto node_l (shared_from_this ());
		auto sock (std::make_shared<boost::asio::ip::tcp::socket> (node_l->io_ctx));
		sock->async_connect (i_a->endpoint (), [node_l, target, body, sock, address, port, i_a, resolver](boost::system::error_code const & ec) mutable {
			if (!ec)
			{
				auto req (std::make_shared<boost::beast::http::request<boost::beast::http::string_body>> ());
				req->method (boost::beast::http::verb::post);
				req->target (*target);
				req->version (11);
				req->insert (boost::beast::http::field::host, address);
				req->insert (boost::beast::http::field::content_type, "application/json");
				req->body () = *body;
				req->prepare_payload ();
				boost::beast::http::async_write (*sock, *req, [node_l, sock, address, port, req, i_a, target, body, resolver](boost::system::error_code const & ec, size_t bytes_transferred) mutable {
					if (!ec)
					{
						auto sb (std::make_shared<boost::beast::flat_buffer> ());
						auto resp (std::make_shared<boost::beast::http::response<boost::beast::http::string_body>> ());
						boost::beast::http::async_read (*sock, *sb, *resp, [node_l, sb, resp, sock, address, port, i_a, target, body, resolver](boost::system::error_code const & ec, size_t bytes_transferred) mutable {
							if (!ec)
							{
								if (boost::beast::http::to_status_class (resp->result ()) == boost::beast::http::status_class::successful)
								{
									node_l->stats.inc (nano::stat::type::http_callback, nano::stat::detail::initiate, nano::stat::dir::out);
								}
								else
								{
									if (node_l->config.logging.callback_logging ())
									{
										node_l->logger.try_log (boost::str (boost::format ("Callback to %1%:%2% failed with status: %3%") % address % port % resp->result ()));
									}
									node_l->stats.inc (nano::stat::type::error, nano::stat::detail::http_callback, nano::stat::dir::out);
								}
							}
							else
							{
								if (node_l->config.logging.callback_logging ())
								{
									node_l->logger.try_log (boost::str (boost::format ("Unable complete callback: %1%:%2%: %3%") % address % port % ec.message ()));
								}
								node_l->stats.inc (nano::stat::type::error, nano::stat::detail::http_callback, nano::stat::dir::out);
							};
						});
					}
					else
					{
						if (node_l->config.logging.callback_logging ())
						{
							node_l->logger.try_log (boost::str (boost::format ("Unable to send callback: %1%:%2%: %3%") % address % port % ec.message ()));
						}
						node_l->stats.inc (nano::stat::type::error, nano::stat::detail::http_callback, nano::stat::dir::out);
					}
				});
			}
			else
			{
				if (node_l->config.logging.callback_logging ())
				{
					node_l->logger.try_log (boost::str (boost::format ("Unable to connect to callback address: %1%:%2%: %3%") % address % port % ec.message ()));
				}
				node_l->stats.inc (nano::stat::type::error, nano::stat::detail::http_callback, nano::stat::dir::out);
				++i_a;
				node_l->do_rpc_callback (i_a, address, port, target, body, resolver);
			}
		});
	}
}

bool nano::node::copy_with_compaction (boost::filesystem::path const & destination)
{
	return store.copy_db (destination);
}

void nano::node::process_fork (nano::transaction const & transaction_a, std::shared_ptr<nano::block> block_a)
{
	auto root (block_a->root ());
	if (!store.block_exists (transaction_a, block_a->type (), block_a->hash ()) && store.root_exists (transaction_a, block_a->root ()))
	{
		std::shared_ptr<nano::block> ledger_block (ledger.forked_block (transaction_a, *block_a));
		if (ledger_block && !block_confirmed_or_being_confirmed (transaction_a, ledger_block->hash ()))
		{
			std::weak_ptr<nano::node> this_w (shared_from_this ());
			if (!active.start (ledger_block, [this_w, root](std::shared_ptr<nano::block>) {
				    if (auto this_l = this_w.lock ())
				    {
					    auto attempt (this_l->bootstrap_initiator.current_attempt ());
					    if (attempt && attempt->mode == nano::bootstrap_mode::legacy)
					    {
						    auto transaction (this_l->store.tx_begin_read ());
						    auto account (this_l->ledger.store.frontier_get (transaction, root));
						    if (!account.is_zero ())
						    {
							    attempt->requeue_pull (nano::pull_info (account, root, root));
						    }
						    else if (this_l->ledger.store.account_exists (transaction, root))
						    {
							    attempt->requeue_pull (nano::pull_info (root, nano::block_hash (0), nano::block_hash (0)));
						    }
					    }
				    }
			    }))
			{
				logger.always_log (boost::str (boost::format ("Resolving fork between our block: %1% and block %2% both with root %3%") % ledger_block->hash ().to_string () % block_a->hash ().to_string () % block_a->root ().to_string ()));
				network.broadcast_confirm_req (ledger_block);
			}
		}
	}
}

namespace nano
{
std::unique_ptr<seq_con_info_component> collect_seq_con_info (node & node, const std::string & name)
{
	auto composite = std::make_unique<seq_con_info_composite> (name);
	composite->add_component (collect_seq_con_info (node.alarm, "alarm"));
	composite->add_component (collect_seq_con_info (node.work, "work"));
	composite->add_component (collect_seq_con_info (node.gap_cache, "gap_cache"));
	composite->add_component (collect_seq_con_info (node.ledger, "ledger"));
	composite->add_component (collect_seq_con_info (node.active, "active"));
	composite->add_component (collect_seq_con_info (node.bootstrap_initiator, "bootstrap_initiator"));
	composite->add_component (collect_seq_con_info (node.bootstrap, "bootstrap"));
	composite->add_component (node.network.tcp_channels.collect_seq_con_info ("tcp_channels"));
	composite->add_component (node.network.udp_channels.collect_seq_con_info ("udp_channels"));
	composite->add_component (node.network.response_channels.collect_seq_con_info ("response_channels"));
	composite->add_component (node.network.syn_cookies.collect_seq_con_info ("syn_cookies"));
	composite->add_component (collect_seq_con_info (node.observers, "observers"));
	composite->add_component (collect_seq_con_info (node.wallets, "wallets"));
	composite->add_component (collect_seq_con_info (node.vote_processor, "vote_processor"));
	composite->add_component (collect_seq_con_info (node.rep_crawler, "rep_crawler"));
	composite->add_component (collect_seq_con_info (node.block_processor, "block_processor"));
	composite->add_component (collect_seq_con_info (node.block_arrival, "block_arrival"));
	composite->add_component (collect_seq_con_info (node.online_reps, "online_reps"));
	composite->add_component (collect_seq_con_info (node.votes_cache, "votes_cache"));
	composite->add_component (collect_seq_con_info (node.block_uniquer, "block_uniquer"));
	composite->add_component (collect_seq_con_info (node.vote_uniquer, "vote_uniquer"));
	composite->add_component (collect_seq_con_info (node.confirmation_height_processor, "confirmation_height_processor"));
	composite->add_component (collect_seq_con_info (node.pending_confirmation_height, "pending_confirmation_height"));
	composite->add_component (collect_seq_con_info (node.worker, "worker"));
	return composite;
}
}

void nano::node::process_active (std::shared_ptr<nano::block> incoming)
{
	block_arrival.add (incoming->hash ());
	block_processor.add (incoming, nano::seconds_since_epoch ());
}

nano::process_return nano::node::process (nano::block const & block_a)
{
	auto transaction (store.tx_begin_write ({ tables::accounts_v0, tables::accounts_v1, tables::cached_counts, tables::change_blocks, tables::frontiers, tables::open_blocks, tables::pending_v0, tables::pending_v1, tables::receive_blocks, tables::representation, tables::send_blocks, tables::state_blocks_v0, tables::state_blocks_v1 }, { tables::confirmation_height }));
	auto result (ledger.process (transaction, block_a));
	return result;
}

nano::process_return nano::node::process_local (std::shared_ptr<nano::block> block_a, bool const work_watcher_a)
{
	// Add block hash as recently arrived to trigger automatic rebroadcast and election
	block_arrival.add (block_a->hash ());
	// Set current time to trigger automatic rebroadcast and election
	nano::unchecked_info info (block_a, block_a->account (), nano::seconds_since_epoch (), nano::signature_verification::unknown);
	// Notify block processor to release write lock
	block_processor.wait_write ();
	// Process block
	auto transaction (store.tx_begin_write ());
	return block_processor.process_one (transaction, info, work_watcher_a);
}

void nano::node::start ()
{
	network.start ();
	add_initial_peers ();
	if (!flags.disable_legacy_bootstrap)
	{
		ongoing_bootstrap ();
	}
	else if (!flags.disable_unchecked_cleanup)
	{
		ongoing_unchecked_cleanup ();
	}
	ongoing_store_flush ();
	rep_crawler.start ();
	ongoing_rep_calculation ();
	ongoing_peer_store ();
	ongoing_online_weight_calculation_queue ();
	if (config.tcp_incoming_connections_max > 0)
	{
		bootstrap.start ();
	}
	if (!flags.disable_backup)
	{
		backup_wallet ();
	}
	search_pending ();
	if (!flags.disable_wallet_bootstrap)
	{
		// Delay to start wallet lazy bootstrap
		auto this_l (shared ());
		alarm.add (std::chrono::steady_clock::now () + std::chrono::minutes (1), [this_l]() {
			this_l->bootstrap_wallet ();
		});
	}
	if (config.external_address != boost::asio::ip::address_v6{} && config.external_port != 0)
	{
		port_mapping.start ();
	}
}

void nano::node::stop ()
{
	if (!stopped.exchange (true))
	{
		logger.always_log ("Node stopping");
		block_processor.stop ();
		if (block_processor_thread.joinable ())
		{
			block_processor_thread.join ();
		}
		vote_processor.stop ();
		confirmation_height_processor.stop ();
		active.stop ();
		network.stop ();
		if (websocket_server)
		{
			websocket_server->stop ();
		}
		bootstrap_initiator.stop ();
		bootstrap.stop ();
		port_mapping.stop ();
		checker.stop ();
		wallets.stop ();
		stats.stop ();
		write_database_queue.stop ();
		worker.stop ();
		// work pool is not stopped on purpose due to testing setup
	}
}

void nano::node::keepalive_preconfigured (std::vector<std::string> const & peers_a)
{
	for (auto i (peers_a.begin ()), n (peers_a.end ()); i != n; ++i)
	{
		keepalive (*i, network_params.network.default_node_port);
	}
}

nano::block_hash nano::node::latest (nano::account const & account_a)
{
	auto transaction (store.tx_begin_read ());
	return ledger.latest (transaction, account_a);
}

nano::uint128_t nano::node::balance (nano::account const & account_a)
{
	auto transaction (store.tx_begin_read ());
	return ledger.account_balance (transaction, account_a);
}

std::shared_ptr<nano::block> nano::node::block (nano::block_hash const & hash_a)
{
	auto transaction (store.tx_begin_read ());
	return store.block_get (transaction, hash_a);
}

std::pair<nano::uint128_t, nano::uint128_t> nano::node::balance_pending (nano::account const & account_a)
{
	std::pair<nano::uint128_t, nano::uint128_t> result;
	auto transaction (store.tx_begin_read ());
	result.first = ledger.account_balance (transaction, account_a);
	result.second = ledger.account_pending (transaction, account_a);
	return result;
}

nano::uint128_t nano::node::weight (nano::account const & account_a)
{
	auto transaction (store.tx_begin_read ());
	return ledger.weight (transaction, account_a);
}

nano::account nano::node::representative (nano::account const & account_a)
{
	auto transaction (store.tx_begin_read ());
	nano::account_info info;
	nano::account result (0);
	if (!store.account_get (transaction, account_a, info))
	{
		result = info.rep_block;
	}
	return result;
}

nano::uint128_t nano::node::minimum_principal_weight ()
{
	return minimum_principal_weight (online_reps.online_stake ());
}

nano::uint128_t nano::node::minimum_principal_weight (nano::uint128_t const & online_stake)
{
	return online_stake / network_params.network.principal_weight_factor;
}

void nano::node::ongoing_rep_calculation ()
{
	auto now (std::chrono::steady_clock::now ());
	vote_processor.calculate_weights ();
	std::weak_ptr<nano::node> node_w (shared_from_this ());
	alarm.add (now + std::chrono::minutes (10), [node_w]() {
		if (auto node_l = node_w.lock ())
		{
			node_l->ongoing_rep_calculation ();
		}
	});
}

void nano::node::ongoing_bootstrap ()
{
	auto next_wakeup (300);
	if (warmed_up < 3)
	{
		// Re-attempt bootstrapping more aggressively on startup
		next_wakeup = 5;
		if (!bootstrap_initiator.in_progress () && !network.empty ())
		{
			++warmed_up;
		}
	}
	bootstrap_initiator.bootstrap ();
	std::weak_ptr<nano::node> node_w (shared_from_this ());
	alarm.add (std::chrono::steady_clock::now () + std::chrono::seconds (next_wakeup), [node_w]() {
		if (auto node_l = node_w.lock ())
		{
			node_l->ongoing_bootstrap ();
		}
	});
}

void nano::node::ongoing_store_flush ()
{
	{
		auto transaction (store.tx_begin_write ({ tables::vote }));
		store.flush (transaction);
	}
	std::weak_ptr<nano::node> node_w (shared_from_this ());
	alarm.add (std::chrono::steady_clock::now () + std::chrono::seconds (5), [node_w]() {
		if (auto node_l = node_w.lock ())
		{
			node_l->worker.push_task ([node_l]() {
				node_l->ongoing_store_flush ();
			});
		}
	});
}

void nano::node::ongoing_peer_store ()
{
	bool stored (network.tcp_channels.store_all (true));
	network.udp_channels.store_all (!stored);
	std::weak_ptr<nano::node> node_w (shared_from_this ());
	alarm.add (std::chrono::steady_clock::now () + network_params.node.peer_interval, [node_w]() {
		if (auto node_l = node_w.lock ())
		{
			node_l->worker.push_task ([node_l]() {
				node_l->ongoing_peer_store ();
			});
		}
	});
}

void nano::node::backup_wallet ()
{
	auto transaction (wallets.tx_begin_read ());
	for (auto i (wallets.items.begin ()), n (wallets.items.end ()); i != n; ++i)
	{
		boost::system::error_code error_chmod;
		auto backup_path (application_path / "backup");

		boost::filesystem::create_directories (backup_path);
		nano::set_secure_perm_directory (backup_path, error_chmod);
		i->second->store.write_backup (transaction, backup_path / (i->first.to_string () + ".json"));
	}
	auto this_l (shared ());
	alarm.add (std::chrono::steady_clock::now () + network_params.node.backup_interval, [this_l]() {
		this_l->backup_wallet ();
	});
}

void nano::node::search_pending ()
{
	// Reload wallets from disk
	wallets.reload ();
	// Search pending
	wallets.search_pending_all ();
	auto this_l (shared ());
	alarm.add (std::chrono::steady_clock::now () + network_params.node.search_pending_interval, [this_l]() {
		this_l->worker.push_task ([this_l]() {
			this_l->search_pending ();
		});
	});
}

void nano::node::bootstrap_wallet ()
{
	std::deque<nano::account> accounts;
	{
		std::lock_guard<std::mutex> lock (wallets.mutex);
		auto transaction (wallets.tx_begin_read ());
		for (auto i (wallets.items.begin ()), n (wallets.items.end ()); i != n && accounts.size () < 128; ++i)
		{
			auto & wallet (*i->second);
			std::lock_guard<std::recursive_mutex> wallet_lock (wallet.store.mutex);
			for (auto j (wallet.store.begin (transaction)), m (wallet.store.end ()); j != m && accounts.size () < 128; ++j)
			{
				nano::account account (j->first);
				accounts.push_back (account);
			}
		}
	}
	bootstrap_initiator.bootstrap_wallet (accounts);
}

void nano::node::unchecked_cleanup ()
{
	std::deque<nano::unchecked_key> cleaning_list;
	// Collect old unchecked keys
	{
		auto now (nano::seconds_since_epoch ());
		auto transaction (store.tx_begin_read ());
		// Max 128k records to clean, max 2 minutes reading to prevent slow i/o systems start issues
		for (auto i (store.unchecked_begin (transaction)), n (store.unchecked_end ()); i != n && cleaning_list.size () < 128 * 1024 && nano::seconds_since_epoch () - now < 120; ++i)
		{
			nano::unchecked_key const & key (i->first);
			nano::unchecked_info const & info (i->second);
			if ((now - info.modified) > static_cast<uint64_t> (config.unchecked_cutoff_time.count ()))
			{
				cleaning_list.push_back (key);
			}
		}
	}
	// Delete old unchecked keys in batches
	while (!cleaning_list.empty ())
	{
		size_t deleted_count (0);
		auto transaction (store.tx_begin_write ());
		while (deleted_count++ < 2 * 1024 && !cleaning_list.empty ())
		{
			auto key (cleaning_list.front ());
			cleaning_list.pop_front ();
			store.unchecked_del (transaction, key);
		}
	}
}

void nano::node::ongoing_unchecked_cleanup ()
{
	if (!bootstrap_initiator.in_progress ())
	{
		unchecked_cleanup ();
	}
	auto this_l (shared ());
	alarm.add (std::chrono::steady_clock::now () + network_params.node.unchecked_cleaning_interval, [this_l]() {
		this_l->worker.push_task ([this_l]() {
			this_l->ongoing_unchecked_cleanup ();
		});
	});
}

int nano::node::price (nano::uint128_t const & balance_a, int amount_a)
{
	assert (balance_a >= amount_a * nano::Gxrb_ratio);
	auto balance_l (balance_a);
	double result (0.0);
	for (auto i (0); i < amount_a; ++i)
	{
		balance_l -= nano::Gxrb_ratio;
		auto balance_scaled ((balance_l / nano::Mxrb_ratio).convert_to<double> ());
		auto units (balance_scaled / 1000.0);
		auto unit_price (((free_cutoff - units) / free_cutoff) * price_max);
		result += std::min (std::max (0.0, unit_price), price_max);
	}
	return static_cast<int> (result * 100.0);
}

namespace
{
class work_request
{
public:
	work_request (boost::asio::io_context & io_ctx_a, boost::asio::ip::address address_a, uint16_t port_a) :
	address (address_a),
	port (port_a),
	socket (io_ctx_a)
	{
	}
	boost::asio::ip::address address;
	uint16_t port;
	boost::beast::flat_buffer buffer;
	boost::beast::http::response<boost::beast::http::string_body> response;
	boost::asio::ip::tcp::socket socket;
};
class distributed_work : public std::enable_shared_from_this<distributed_work>
{
public:
	distributed_work (std::shared_ptr<nano::node> const & node_a, nano::block_hash const & root_a, std::function<void(uint64_t)> const & callback_a, uint64_t difficulty_a) :
	distributed_work (1, node_a, root_a, callback_a, difficulty_a)
	{
		assert (node_a != nullptr);
	}
	~distributed_work ()
	{
		stop (true);
	}
	distributed_work (unsigned int backoff_a, std::shared_ptr<nano::node> const & node_a, nano::block_hash const & root_a, std::function<void(uint64_t)> const & callback_a, uint64_t difficulty_a) :
	callback (callback_a),
	backoff (backoff_a),
	node (node_a),
	root (root_a),
	need_resolve (node_a->config.work_peers),
	difficulty (difficulty_a)
	{
		assert (node_a != nullptr);
		assert (!completed);
	}
	void start ()
	{
		if (need_resolve.empty ())
		{
			start_work ();
		}
		else
		{
			auto current (need_resolve.back ());
			need_resolve.pop_back ();
			auto this_l (shared_from_this ());
			boost::system::error_code ec;
			auto parsed_address (boost::asio::ip::address_v6::from_string (current.first, ec));
			if (!ec)
			{
				outstanding[parsed_address] = current.second;
				start ();
			}
			else
			{
				node->network.resolver.async_resolve (boost::asio::ip::udp::resolver::query (current.first, std::to_string (current.second)), [current, this_l](boost::system::error_code const & ec, boost::asio::ip::udp::resolver::iterator i_a) {
					if (!ec)
					{
						for (auto i (i_a), n (boost::asio::ip::udp::resolver::iterator{}); i != n; ++i)
						{
							auto endpoint (i->endpoint ());
							this_l->outstanding[endpoint.address ()] = endpoint.port ();
						}
					}
					else
					{
						this_l->node->logger.try_log (boost::str (boost::format ("Error resolving work peer: %1%:%2%: %3%") % current.first % current.second % ec.message ()));
					}
					this_l->start ();
				});
			}
		}
	}
	void start_work ()
	{
		auto this_l (shared_from_this ());

		// Start work generation if peers are not acting correctly, or if there are no peers configured
		if ((outstanding.empty () || node->unresponsive_work_peers) && (node->config.work_threads != 0 || node->work.opencl))
		{
			local_generation_started = true;
			node->work.generate (
			this_l->root, [this_l](boost::optional<uint64_t> const & work_a) {
				if (work_a)
				{
					this_l->set_once (work_a.value ());
					this_l->stop (false);
				}
			},
			difficulty);
		}

		if (!outstanding.empty ())
		{
			std::lock_guard<std::mutex> guard (mutex);
			for (auto const & i : outstanding)
			{
				auto host (i.first);
				auto service (i.second);
				auto connection (std::make_shared<work_request> (this_l->node->io_ctx, host, service));
				connections.push_back (connection);
				connection->socket.async_connect (nano::tcp_endpoint (host, service), [this_l, connection](boost::system::error_code const & ec) {
					if (!ec)
					{
						std::string request_string;
						{
							boost::property_tree::ptree request;
							request.put ("action", "work_generate");
							request.put ("hash", this_l->root.to_string ());
							request.put ("difficulty", nano::to_string_hex (this_l->difficulty));
							std::stringstream ostream;
							boost::property_tree::write_json (ostream, request);
							request_string = ostream.str ();
						}
						auto request (std::make_shared<boost::beast::http::request<boost::beast::http::string_body>> ());
						request->method (boost::beast::http::verb::post);
						request->set (boost::beast::http::field::content_type, "application/json");
						request->target ("/");
						request->version (11);
						request->body () = request_string;
						request->prepare_payload ();
						boost::beast::http::async_write (connection->socket, *request, [this_l, connection, request](boost::system::error_code const & ec, size_t bytes_transferred) {
							if (!ec)
							{
								boost::beast::http::async_read (connection->socket, connection->buffer, connection->response, [this_l, connection](boost::system::error_code const & ec, size_t bytes_transferred) {
									if (!ec)
									{
										if (connection->response.result () == boost::beast::http::status::ok)
										{
											this_l->success (connection->response.body (), connection->address);
										}
										else
										{
											this_l->node->logger.try_log (boost::str (boost::format ("Work peer responded with an error %1% %2%: %3%") % connection->address % connection->port % connection->response.result ()));
											this_l->failure (connection->address);
										}
									}
									else if (ec == boost::system::errc::operation_canceled)
									{
										// The only case where we send a cancel is if we preempt stopped waiting for the response
										this_l->cancel (connection);
										this_l->failure (connection->address);
									}
									else
									{
										this_l->node->logger.try_log (boost::str (boost::format ("Unable to read from work_peer %1% %2%: %3% (%4%)") % connection->address % connection->port % ec.message () % ec.value ()));
										this_l->failure (connection->address);
									}
								});
							}
							else
							{
								this_l->node->logger.try_log (boost::str (boost::format ("Unable to write to work_peer %1% %2%: %3% (%4%)") % connection->address % connection->port % ec.message () % ec.value ()));
								this_l->failure (connection->address);
							}
						});
					}
					else
					{
						this_l->node->logger.try_log (boost::str (boost::format ("Unable to connect to work_peer %1% %2%: %3% (%4%)") % connection->address % connection->port % ec.message () % ec.value ()));
						this_l->failure (connection->address);
					}
				});
			}
		}
	}
	void cancel (std::shared_ptr<work_request> connection)
	{
		auto this_l (shared_from_this ());
		auto cancelling (std::make_shared<work_request> (node->io_ctx, connection->address, connection->port));
		cancelling->socket.async_connect (nano::tcp_endpoint (cancelling->address, cancelling->port), [this_l, cancelling](boost::system::error_code const & ec) {
			if (!ec)
			{
				std::string request_string;
				{
					boost::property_tree::ptree request;
					request.put ("action", "work_cancel");
					request.put ("hash", this_l->root.to_string ());
					std::stringstream ostream;
					boost::property_tree::write_json (ostream, request);
					request_string = ostream.str ();
				}
				auto request (std::make_shared<boost::beast::http::request<boost::beast::http::string_body>> ());
				request->method (boost::beast::http::verb::post);
				request->set (boost::beast::http::field::content_type, "application/json");
				request->target ("/");
				request->version (11);
				request->body () = request_string;
				request->prepare_payload ();

				boost::beast::http::async_write (cancelling->socket, *request, [this_l, request, cancelling](boost::system::error_code const & ec, size_t bytes_transferred) {
					if (ec)
					{
						this_l->node->logger.try_log (boost::str (boost::format ("Unable to send work_cancel to work_peer %1% %2%: %3% (%4%)") % cancelling->address % cancelling->port % ec.message () % ec.value ()));
					}
				});
			}
		});
	}
	void stop (bool const local_stop)
	{
		if (!stopped.exchange (true))
		{
			std::lock_guard<std::mutex> lock (mutex);
			if (local_stop && (node->config.work_threads != 0 || node->work.opencl))
			{
				node->work.cancel (root);
			}
			for (auto & i : connections)
			{
				auto connection = i.lock ();
				if (connection)
				{
					boost::system::error_code ec;
					connection->socket.cancel (ec);
					if (ec)
					{
						node->logger.try_log (boost::str (boost::format ("Error cancelling operation with work_peer %1% %2%: %3%") % connection->address % connection->port % ec.message () % ec.value ()));
					}
					try
					{
						connection->socket.close ();
					}
					catch (const boost::system::system_error & ec)
					{
						node->logger.try_log (boost::str (boost::format ("Error closing socket with work_peer %1% %2%: %3%") % connection->address % connection->port % ec.what () % ec.code ()));
					}
				}
			}
			connections.clear ();
			outstanding.clear ();
		}
	}
	void success (std::string const & body_a, boost::asio::ip::address const & address)
	{
		auto last (remove (address));
		std::stringstream istream (body_a);
		try
		{
			boost::property_tree::ptree result;
			boost::property_tree::read_json (istream, result);
			auto work_text (result.get<std::string> ("work"));
			uint64_t work;
			if (!nano::from_string_hex (work_text, work))
			{
				uint64_t result_difficulty (0);
				if (!nano::work_validate (root, work, &result_difficulty) && result_difficulty >= difficulty)
				{
					node->unresponsive_work_peers = false;
					set_once (work);
					stop (true);
				}
				else
				{
					node->logger.try_log (boost::str (boost::format ("Incorrect work response from %1% for root %2% with diffuculty %3%: %4%") % address % root.to_string () % nano::to_string_hex (difficulty) % work_text));
					handle_failure (last);
				}
			}
			else
			{
				node->logger.try_log (boost::str (boost::format ("Work response from %1% wasn't a number: %2%") % address % work_text));
				handle_failure (last);
			}
		}
		catch (...)
		{
			node->logger.try_log (boost::str (boost::format ("Work response from %1% wasn't parsable: %2%") % address % body_a));
			handle_failure (last);
		}
	}
	void set_once (uint64_t work_a)
	{
		if (!completed.exchange (true))
		{
			callback (work_a);
		}
	}
	void failure (boost::asio::ip::address const & address)
	{
		auto last (remove (address));
		handle_failure (last);
	}
	void handle_failure (bool last)
	{
		if (last)
		{
			if (!completed)
			{
				node->unresponsive_work_peers = true;
				if (!local_generation_started)
				{
					if (backoff == 1 && node->config.logging.work_generation_time ())
					{
						node->logger.always_log ("Work peer(s) failed to generate work for root ", root.to_string (), ", retrying...");
					}
					auto now (std::chrono::steady_clock::now ());
					auto root_l (root);
					auto callback_l (callback);
					std::weak_ptr<nano::node> node_w (node);
					auto next_backoff (std::min (backoff * 2, (unsigned int)60 * 5));
					// clang-format off
					node->alarm.add (now + std::chrono::seconds (backoff), [ node_w, root_l, callback_l, next_backoff, difficulty = difficulty ] {
						if (auto node_l = node_w.lock ())
						{
							auto work_generation (std::make_shared<distributed_work> (next_backoff, node_l, root_l, callback_l, difficulty));
							work_generation->start ();
						}
					});
					// clang-format on
				}
			}
		}
	}
	bool remove (boost::asio::ip::address const & address)
	{
		std::lock_guard<std::mutex> lock (mutex);
		outstanding.erase (address);
		return outstanding.empty ();
	}
	std::function<void(uint64_t)> callback;
	unsigned int backoff; // in seconds
	std::shared_ptr<nano::node> node;
	nano::block_hash root;
	std::mutex mutex;
	std::map<boost::asio::ip::address, uint16_t> outstanding;
	std::vector<std::weak_ptr<work_request>> connections;
	std::vector<std::pair<std::string, uint16_t>> need_resolve;
	uint64_t difficulty;
	std::atomic<bool> completed{ false };
	std::atomic<bool> local_generation_started{ false };
	std::atomic<bool> stopped{ false };
};
}

void nano::node::work_generate_blocking (nano::block & block_a)
{
	work_generate_blocking (block_a, network_params.network.publish_threshold);
}

void nano::node::work_generate_blocking (nano::block & block_a, uint64_t difficulty_a)
{
	block_a.block_work_set (work_generate_blocking (block_a.root (), difficulty_a));
}

void nano::node::work_generate (nano::uint256_union const & hash_a, std::function<void(uint64_t)> callback_a)
{
	work_generate (hash_a, callback_a, network_params.network.publish_threshold);
}

void nano::node::work_generate (nano::uint256_union const & hash_a, std::function<void(uint64_t)> callback_a, uint64_t difficulty_a)
{
	auto work_generation (std::make_shared<distributed_work> (shared (), hash_a, callback_a, difficulty_a));
	work_generation->start ();
}

uint64_t nano::node::work_generate_blocking (nano::uint256_union const & block_a)
{
	return work_generate_blocking (block_a, network_params.network.publish_threshold);
}

uint64_t nano::node::work_generate_blocking (nano::uint256_union const & hash_a, uint64_t difficulty_a)
{
	std::promise<uint64_t> promise;
	std::future<uint64_t> future = promise.get_future ();
	// clang-format off
	work_generate (hash_a, [&promise](uint64_t work_a) {
		promise.set_value (work_a);
	},
	difficulty_a);
	// clang-format on
	return future.get ();
}

void nano::node::add_initial_peers ()
{
	auto transaction (store.tx_begin_read ());
	for (auto i (store.peers_begin (transaction)), n (store.peers_end ()); i != n; ++i)
	{
		nano::endpoint endpoint (boost::asio::ip::address_v6 (i->first.address_bytes ()), i->first.port ());
		if (!network.reachout (endpoint, config.allow_local_peers))
		{
			std::weak_ptr<nano::node> node_w (shared_from_this ());
			network.tcp_channels.start_tcp (endpoint, [node_w](std::shared_ptr<nano::transport::channel> channel_a) {
				if (auto node_l = node_w.lock ())
				{
					node_l->network.send_keepalive (channel_a);
					node_l->rep_crawler.query (channel_a);
				}
			});
		}
	}
}

void nano::node::block_confirm (std::shared_ptr<nano::block> block_a)
{
	active.start (block_a);
	network.broadcast_confirm_req (block_a);
	// Calculate votes for local representatives
	if (config.enable_voting && active.active (*block_a))
	{
		block_processor.generator.add (block_a->hash ());
	}
}

bool nano::node::block_confirmed_or_being_confirmed (nano::transaction const & transaction_a, nano::block_hash const & hash_a)
{
	return ledger.block_confirmed (transaction_a, hash_a) || pending_confirmation_height.is_processing_block (hash_a);
}

nano::uint128_t nano::node::delta () const
{
	auto result ((online_reps.online_stake () / 100) * config.online_weight_quorum);
	return result;
}

void nano::node::ongoing_online_weight_calculation_queue ()
{
	std::weak_ptr<nano::node> node_w (shared_from_this ());
	alarm.add (std::chrono::steady_clock::now () + (std::chrono::seconds (network_params.node.weight_period)), [node_w]() {
		if (auto node_l = node_w.lock ())
		{
			node_l->worker.push_task ([node_l]() {
				node_l->ongoing_online_weight_calculation ();
			});
		}
	});
}

bool nano::node::online () const
{
	return rep_crawler.total_weight () > (std::max (config.online_weight_minimum.number (), delta ()));
}

void nano::node::ongoing_online_weight_calculation ()
{
	online_reps.sample ();
	ongoing_online_weight_calculation_queue ();
}

namespace
{
class confirmed_visitor : public nano::block_visitor
{
public:
	confirmed_visitor (nano::transaction const & transaction_a, nano::node & node_a, std::shared_ptr<nano::block> const & block_a, nano::block_hash const & hash_a) :
	transaction (transaction_a),
	node (node_a),
	block (block_a),
	hash (hash_a)
	{
	}
	virtual ~confirmed_visitor () = default;
	void scan_receivable (nano::account const & account_a)
	{
		for (auto i (node.wallets.items.begin ()), n (node.wallets.items.end ()); i != n; ++i)
		{
			auto const & wallet (i->second);
			auto transaction_l (node.wallets.tx_begin_read ());
			if (wallet->store.exists (transaction_l, account_a))
			{
				nano::account representative;
				nano::pending_info pending;
				representative = wallet->store.representative (transaction_l);
				auto error (node.store.pending_get (transaction, nano::pending_key (account_a, hash), pending));
				if (!error)
				{
					auto node_l (node.shared ());
					auto amount (pending.amount.number ());
					wallet->receive_async (block, representative, amount, [](std::shared_ptr<nano::block>) {});
				}
				else
				{
					if (!node.store.block_exists (transaction, hash))
					{
						node.logger.try_log (boost::str (boost::format ("Confirmed block is missing:  %1%") % hash.to_string ()));
						assert (false && "Confirmed block is missing");
					}
					else
					{
						node.logger.try_log (boost::str (boost::format ("Block %1% has already been received") % hash.to_string ()));
					}
				}
			}
		}
	}
	void state_block (nano::state_block const & block_a) override
	{
		scan_receivable (block_a.hashables.link);
	}
	void send_block (nano::send_block const & block_a) override
	{
		scan_receivable (block_a.hashables.destination);
	}
	void receive_block (nano::receive_block const &) override
	{
	}
	void open_block (nano::open_block const &) override
	{
	}
	void change_block (nano::change_block const &) override
	{
	}
	nano::transaction const & transaction;
	nano::node & node;
	std::shared_ptr<nano::block> block;
	nano::block_hash const & hash;
};
}

void nano::node::receive_confirmed (nano::transaction const & transaction_a, std::shared_ptr<nano::block> block_a, nano::block_hash const & hash_a)
{
	confirmed_visitor visitor (transaction_a, *this, block_a, hash_a);
	block_a->visit (visitor);
}

void nano::node::process_confirmed_data (nano::transaction const & transaction_a, std::shared_ptr<nano::block> block_a, nano::block_hash const & hash_a, nano::block_sideband const & sideband_a, nano::account & account_a, nano::uint128_t & amount_a, bool & is_state_send_a, nano::account & pending_account_a)
{
	// Faster account calculation
	account_a = block_a->account ();
	if (account_a.is_zero ())
	{
		account_a = sideband_a.account;
	}
	// Faster amount calculation
	auto previous (block_a->previous ());
	auto previous_balance (ledger.balance (transaction_a, previous));
	auto block_balance (store.block_balance_calculated (block_a, sideband_a));
	if (hash_a != ledger.network_params.ledger.genesis_account)
	{
		amount_a = block_balance > previous_balance ? block_balance - previous_balance : previous_balance - block_balance;
	}
	else
	{
		amount_a = ledger.network_params.ledger.genesis_amount;
	}
	if (auto state = dynamic_cast<nano::state_block *> (block_a.get ()))
	{
		if (state->hashables.balance < previous_balance)
		{
			is_state_send_a = true;
		}
		pending_account_a = state->hashables.link;
	}
	if (auto send = dynamic_cast<nano::send_block *> (block_a.get ()))
	{
		pending_account_a = send->hashables.destination;
	}
}

void nano::node::process_confirmed (nano::election_status const & status_a, uint8_t iteration)
{
	auto block_a (status_a.winner);
	auto hash (block_a->hash ());
	nano::block_sideband sideband;
	auto transaction (store.tx_begin_read ());
	if (store.block_get (transaction, hash, &sideband) != nullptr)
	{
		confirmation_height_processor.add (hash);

		receive_confirmed (transaction, block_a, hash);
		nano::account account (0);
		nano::uint128_t amount (0);
		bool is_state_send (false);
		nano::account pending_account (0);
		process_confirmed_data (transaction, block_a, hash, sideband, account, amount, is_state_send, pending_account);
		observers.blocks.notify (status_a, account, amount, is_state_send);
		if (amount > 0)
		{
			observers.account_balance.notify (account, false);
			if (!pending_account.is_zero ())
			{
				observers.account_balance.notify (pending_account, true);
			}
		}
	}
	// Limit to 0.5 * 20 = 10 seconds (more than max block_processor::process_batch finish time)
	else if (iteration < 20)
	{
		iteration++;
		std::weak_ptr<nano::node> node_w (shared ());
		alarm.add (std::chrono::steady_clock::now () + network_params.node.process_confirmed_interval, [node_w, status_a, iteration]() {
			if (auto node_l = node_w.lock ())
			{
				node_l->process_confirmed (status_a, iteration);
			}
		});
	}
}

bool nano::block_arrival::add (nano::block_hash const & hash_a)
{
	std::lock_guard<std::mutex> lock (mutex);
	auto now (std::chrono::steady_clock::now ());
	auto inserted (arrival.insert (nano::block_arrival_info{ now, hash_a }));
	auto result (!inserted.second);
	return result;
}

bool nano::block_arrival::recent (nano::block_hash const & hash_a)
{
	std::lock_guard<std::mutex> lock (mutex);
	auto now (std::chrono::steady_clock::now ());
	while (arrival.size () > arrival_size_min && arrival.begin ()->arrival + arrival_time_min < now)
	{
		arrival.erase (arrival.begin ());
	}
	return arrival.get<1> ().find (hash_a) != arrival.get<1> ().end ();
}

namespace nano
{
std::unique_ptr<seq_con_info_component> collect_seq_con_info (block_arrival & block_arrival, const std::string & name)
{
	size_t count = 0;
	{
		std::lock_guard<std::mutex> guard (block_arrival.mutex);
		count = block_arrival.arrival.size ();
	}

	auto sizeof_element = sizeof (decltype (block_arrival.arrival)::value_type);
	auto composite = std::make_unique<seq_con_info_composite> (name);
	composite->add_component (std::make_unique<seq_con_info_leaf> (seq_con_info{ "arrival", count, sizeof_element }));
	return composite;
}
}

std::shared_ptr<nano::node> nano::node::shared ()
{
	return shared_from_this ();
}

bool nano::node::validate_block_by_previous (nano::transaction const & transaction, std::shared_ptr<nano::block> block_a)
{
	bool result (false);
	nano::account account;
	if (!block_a->previous ().is_zero ())
	{
		if (store.block_exists (transaction, block_a->previous ()))
		{
			account = ledger.account (transaction, block_a->previous ());
		}
		else
		{
			result = true;
		}
	}
	else
	{
		account = block_a->root ();
	}
	if (!result && block_a->type () == nano::block_type::state)
	{
		std::shared_ptr<nano::state_block> block_l (std::static_pointer_cast<nano::state_block> (block_a));
		nano::amount prev_balance (0);
		if (!block_l->hashables.previous.is_zero ())
		{
			if (store.block_exists (transaction, block_l->hashables.previous))
			{
				prev_balance = ledger.balance (transaction, block_l->hashables.previous);
			}
			else
			{
				result = true;
			}
		}
		if (!result)
		{
			if (block_l->hashables.balance == prev_balance && !ledger.epoch_link.is_zero () && ledger.is_epoch_link (block_l->hashables.link))
			{
				account = ledger.epoch_signer;
			}
		}
	}
	if (!result && (account.is_zero () || nano::validate_message (account, block_a->hash (), block_a->block_signature ())))
	{
		result = true;
	}
	return result;
}

int nano::node::store_version ()
{
	auto transaction (store.tx_begin_read ());
	return store.version_get (transaction);
}

bool nano::node::init_error () const
{
	return store.init_error () || wallets_store.init_error ();
}

nano::inactive_node::inactive_node (boost::filesystem::path const & path_a, uint16_t peering_port_a, bool read_only_a, bool cache_reps_a) :
path (path_a),
io_context (std::make_shared<boost::asio::io_context> ()),
alarm (*io_context),
work (1),
peering_port (peering_port_a)
{
	boost::system::error_code error_chmod;

	/*
	 * @warning May throw a filesystem exception
	 */
	boost::filesystem::create_directories (path);
	nano::set_secure_perm_directory (path, error_chmod);
	logging.max_size = std::numeric_limits<std::uintmax_t>::max ();
	logging.init (path);
	nano::node_flags node_flags;
	node_flags.read_only = read_only_a;
	node_flags.cache_representative_weights_from_frontiers = cache_reps_a;
	node = std::make_shared<nano::node> (*io_context, peering_port, path, alarm, logging, work, node_flags);
	node->active.stop ();
}

nano::inactive_node::~inactive_node ()
{
	node->stop ();
}

std::unique_ptr<nano::block_store> nano::make_store (nano::logger_mt & logger, boost::filesystem::path const & path, bool read_only, bool add_db_postfix, nano::txn_tracking_config const & txn_tracking_config_a, std::chrono::milliseconds block_processor_batch_max_time_a, int lmdb_max_dbs, bool drop_unchecked, size_t batch_size, bool backup_before_upgrade)
{
#if NANO_ROCKSDB
	return std::make_unique<nano::rocksdb_store> (logger, add_db_postfix ? path / "rocksdb" : path, drop_unchecked, read_only);
#else
	return std::make_unique<nano::mdb_store> (logger, add_db_postfix ? path / "data.ldb" : path, txn_tracking_config_a, block_processor_batch_max_time_a, lmdb_max_dbs, drop_unchecked, batch_size, backup_before_upgrade);
#endif
}
