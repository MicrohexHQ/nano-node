#include <nano/crypto_lib/random_pool.hpp>
#include <nano/node/bootstrap.hpp>
#include <nano/node/common.hpp>
#include <nano/node/node.hpp>
#include <nano/node/transport/tcp.hpp>
#include <nano/node/transport/udp.hpp>

#include <boost/log/trivial.hpp>

#include <algorithm>

constexpr double bootstrap_connection_scale_target_blocks = 50000.0;
constexpr double bootstrap_connection_warmup_time_sec = 5.0;
constexpr double bootstrap_minimum_blocks_per_sec = 10.0;
constexpr double bootstrap_minimum_elapsed_seconds_blockrate = 0.02;
constexpr double bootstrap_minimum_frontier_blocks_per_sec = 1000.0;
constexpr unsigned bootstrap_frontier_retry_limit = 16;
constexpr double bootstrap_minimum_termination_time_sec = 30.0;
constexpr unsigned bootstrap_max_new_connections = 10;
constexpr unsigned bulk_push_cost_limit = 200;

size_t constexpr nano::frontier_req_client::size_frontier;

nano::bootstrap_client::bootstrap_client (std::shared_ptr<nano::node> node_a, std::shared_ptr<nano::bootstrap_attempt> attempt_a, std::shared_ptr<nano::transport::channel_tcp> channel_a) :
node (node_a),
attempt (attempt_a),
channel (channel_a),
receive_buffer (std::make_shared<std::vector<uint8_t>> ()),
start_time (std::chrono::steady_clock::now ()),
block_count (0),
pending_stop (false),
hard_stop (false)
{
	++attempt->connections;
	receive_buffer->resize (256);
}

nano::bootstrap_client::~bootstrap_client ()
{
	--attempt->connections;
}

double nano::bootstrap_client::block_rate () const
{
	auto elapsed = std::max (elapsed_seconds (), bootstrap_minimum_elapsed_seconds_blockrate);
	return static_cast<double> (block_count.load () / elapsed);
}

double nano::bootstrap_client::elapsed_seconds () const
{
	return std::chrono::duration_cast<std::chrono::duration<double>> (std::chrono::steady_clock::now () - start_time).count ();
}

void nano::bootstrap_client::stop (bool force)
{
	pending_stop = true;
	if (force)
	{
		hard_stop = true;
	}
}

void nano::frontier_req_client::run ()
{
	nano::frontier_req request;
	request.start.clear ();
	request.age = std::numeric_limits<decltype (request.age)>::max ();
	request.count = std::numeric_limits<decltype (request.count)>::max ();
	auto this_l (shared_from_this ());
	connection->channel->send (
	request, [this_l](boost::system::error_code const & ec, size_t size_a) {
		if (!ec)
		{
			this_l->receive_frontier ();
		}
		else
		{
			if (this_l->connection->node->config.logging.network_logging ())
			{
				this_l->connection->node->logger.try_log (boost::str (boost::format ("Error while sending bootstrap request %1%") % ec.message ()));
			}
		}
	},
	false); // is bootstrap traffic is_droppable false
}

std::shared_ptr<nano::bootstrap_client> nano::bootstrap_client::shared ()
{
	return shared_from_this ();
}

nano::frontier_req_client::frontier_req_client (std::shared_ptr<nano::bootstrap_client> connection_a) :
connection (connection_a),
current (0),
count (0),
bulk_push_cost (0)
{
	auto transaction (connection->node->store.tx_begin_read ());
	next (transaction);
}

nano::frontier_req_client::~frontier_req_client ()
{
}

void nano::frontier_req_client::receive_frontier ()
{
	auto this_l (shared_from_this ());
	connection->channel->socket->async_read (connection->receive_buffer, nano::frontier_req_client::size_frontier, [this_l](boost::system::error_code const & ec, size_t size_a) {
		// An issue with asio is that sometimes, instead of reporting a bad file descriptor during disconnect,
		// we simply get a size of 0.
		if (size_a == nano::frontier_req_client::size_frontier)
		{
			this_l->received_frontier (ec, size_a);
		}
		else
		{
			if (this_l->connection->node->config.logging.network_message_logging ())
			{
				this_l->connection->node->logger.try_log (boost::str (boost::format ("Invalid size: expected %1%, got %2%") % nano::frontier_req_client::size_frontier % size_a));
			}
		}
	});
}

void nano::frontier_req_client::unsynced (nano::block_hash const & head, nano::block_hash const & end)
{
	if (bulk_push_cost < bulk_push_cost_limit)
	{
		connection->attempt->add_bulk_push_target (head, end);
		if (end.is_zero ())
		{
			bulk_push_cost += 2;
		}
		else
		{
			bulk_push_cost += 1;
		}
	}
}

void nano::frontier_req_client::received_frontier (boost::system::error_code const & ec, size_t size_a)
{
	if (!ec)
	{
		assert (size_a == nano::frontier_req_client::size_frontier);
		nano::account account;
		nano::bufferstream account_stream (connection->receive_buffer->data (), sizeof (account));
		auto error1 (nano::try_read (account_stream, account));
		(void)error1;
		assert (!error1);
		nano::block_hash latest;
		nano::bufferstream latest_stream (connection->receive_buffer->data () + sizeof (account), sizeof (latest));
		auto error2 (nano::try_read (latest_stream, latest));
		(void)error2;
		assert (!error2);
		if (count == 0)
		{
			start_time = std::chrono::steady_clock::now ();
		}
		++count;
		std::chrono::duration<double> time_span = std::chrono::duration_cast<std::chrono::duration<double>> (std::chrono::steady_clock::now () - start_time);

		double elapsed_sec = std::max (time_span.count (), bootstrap_minimum_elapsed_seconds_blockrate);
		double blocks_per_sec = static_cast<double> (count) / elapsed_sec;
		if (elapsed_sec > bootstrap_connection_warmup_time_sec && blocks_per_sec < bootstrap_minimum_frontier_blocks_per_sec)
		{
			connection->node->logger.try_log (boost::str (boost::format ("Aborting frontier req because it was too slow")));
			promise.set_value (true);
			return;
		}
		if (connection->attempt->should_log ())
		{
			connection->node->logger.always_log (boost::str (boost::format ("Received %1% frontiers from %2%") % std::to_string (count) % connection->channel->to_string ()));
		}
		auto transaction (connection->node->store.tx_begin_read ());
		if (!account.is_zero ())
		{
			while (!current.is_zero () && current < account)
			{
				// We know about an account they don't.
				unsynced (frontier, 0);
				next (transaction);
			}
			if (!current.is_zero ())
			{
				if (account == current)
				{
					if (latest == frontier)
					{
						// In sync
					}
					else
					{
						if (connection->node->store.block_exists (transaction, latest))
						{
							// We know about a block they don't.
							unsynced (frontier, latest);
						}
						else
						{
							connection->attempt->add_pull (nano::pull_info (account, latest, frontier));
							// Either we're behind or there's a fork we differ on
							// Either way, bulk pushing will probably not be effective
							bulk_push_cost += 5;
						}
					}
					next (transaction);
				}
				else
				{
					assert (account < current);
					connection->attempt->add_pull (nano::pull_info (account, latest, nano::block_hash (0)));
				}
			}
			else
			{
				connection->attempt->add_pull (nano::pull_info (account, latest, nano::block_hash (0)));
			}
			receive_frontier ();
		}
		else
		{
			while (!current.is_zero ())
			{
				// We know about an account they don't.
				unsynced (frontier, 0);
				next (transaction);
			}
			if (connection->node->config.logging.bulk_pull_logging ())
			{
				connection->node->logger.try_log ("Bulk push cost: ", bulk_push_cost);
			}
			{
				try
				{
					promise.set_value (false);
				}
				catch (std::future_error &)
				{
				}
				connection->attempt->pool_connection (connection);
			}
		}
	}
	else
	{
		if (connection->node->config.logging.network_logging ())
		{
			connection->node->logger.try_log (boost::str (boost::format ("Error while receiving frontier %1%") % ec.message ()));
		}
	}
}

void nano::frontier_req_client::next (nano::transaction const & transaction_a)
{
	// Filling accounts deque to prevent often read transactions
	if (accounts.empty ())
	{
		size_t max_size (128);
		for (auto i (connection->node->store.latest_begin (transaction_a, current.number () + 1)), n (connection->node->store.latest_end ()); i != n && accounts.size () != max_size; ++i)
		{
			nano::account_info const & info (i->second);
			nano::account const & account (i->first);
			accounts.emplace_back (account, info.head);
		}
		/* If loop breaks before max_size, then latest_end () is reached
		Add empty record to finish frontier_req_server */
		if (accounts.size () != max_size)
		{
			accounts.emplace_back (nano::account (0), nano::block_hash (0));
		}
	}
	// Retrieving accounts from deque
	auto const & account_pair (accounts.front ());
	current = account_pair.first;
	frontier = account_pair.second;
	accounts.pop_front ();
}

nano::bulk_pull_client::bulk_pull_client (std::shared_ptr<nano::bootstrap_client> connection_a, nano::pull_info const & pull_a) :
connection (connection_a),
known_account (0),
pull (pull_a),
pull_blocks (0),
unexpected_count (0)
{
	std::lock_guard<std::mutex> mutex (connection->attempt->mutex);
	connection->attempt->condition.notify_all ();
}

nano::bulk_pull_client::~bulk_pull_client ()
{
	// If received end block is not expected end block
	if (expected != pull.end)
	{
		pull.head = expected;
		if (connection->attempt->mode != nano::bootstrap_mode::legacy)
		{
			pull.account = expected;
		}
		pull.processed += pull_blocks - unexpected_count;
		connection->attempt->requeue_pull (pull);
		if (connection->node->config.logging.bulk_pull_logging ())
		{
			connection->node->logger.try_log (boost::str (boost::format ("Bulk pull end block is not expected %1% for account %2%") % pull.end.to_string () % pull.account.to_account ()));
		}
	}
	else
	{
		connection->node->bootstrap_initiator.cache.remove (pull);
	}
	{
		std::lock_guard<std::mutex> mutex (connection->attempt->mutex);
		--connection->attempt->pulling;
	}
	connection->attempt->condition.notify_all ();
}

void nano::bulk_pull_client::request ()
{
	expected = pull.head;
	nano::bulk_pull req;
	req.start = (pull.head == pull.head_original) ? pull.account : pull.head; // Account for new pulls, head for cached pulls
	req.end = pull.end;
	req.count = pull.count;
	req.set_count_present (pull.count != 0);

	if (connection->node->config.logging.bulk_pull_logging ())
	{
		std::unique_lock<std::mutex> lock (connection->attempt->mutex);
		connection->node->logger.try_log (boost::str (boost::format ("Requesting account %1% from %2%. %3% accounts in queue") % pull.account.to_account () % connection->channel->to_string () % connection->attempt->pulls.size ()));
	}
	else if (connection->node->config.logging.network_logging () && connection->attempt->should_log ())
	{
		std::unique_lock<std::mutex> lock (connection->attempt->mutex);
		connection->node->logger.always_log (boost::str (boost::format ("%1% accounts in pull queue") % connection->attempt->pulls.size ()));
	}
	auto this_l (shared_from_this ());
	connection->channel->send (
	req, [this_l](boost::system::error_code const & ec, size_t size_a) {
		if (!ec)
		{
			this_l->throttled_receive_block ();
		}
		else
		{
			if (this_l->connection->node->config.logging.bulk_pull_logging ())
			{
				this_l->connection->node->logger.try_log (boost::str (boost::format ("Error sending bulk pull request to %1%: to %2%") % ec.message () % this_l->connection->channel->to_string ()));
			}
			this_l->connection->node->stats.inc (nano::stat::type::bootstrap, nano::stat::detail::bulk_pull_request_failure, nano::stat::dir::in);
		}
	},
	false); // is bootstrap traffic is_droppable false
}

void nano::bulk_pull_client::throttled_receive_block ()
{
	if (!connection->node->block_processor.half_full ())
	{
		receive_block ();
	}
	else
	{
		auto this_l (shared_from_this ());
		connection->node->alarm.add (std::chrono::steady_clock::now () + std::chrono::seconds (1), [this_l]() {
			if (!this_l->connection->pending_stop && !this_l->connection->attempt->stopped)
			{
				this_l->throttled_receive_block ();
			}
		});
	}
}

void nano::bulk_pull_client::receive_block ()
{
	auto this_l (shared_from_this ());
	connection->channel->socket->async_read (connection->receive_buffer, 1, [this_l](boost::system::error_code const & ec, size_t size_a) {
		if (!ec)
		{
			this_l->received_type ();
		}
		else
		{
			if (this_l->connection->node->config.logging.bulk_pull_logging ())
			{
				this_l->connection->node->logger.try_log (boost::str (boost::format ("Error receiving block type: %1%") % ec.message ()));
			}
			this_l->connection->node->stats.inc (nano::stat::type::bootstrap, nano::stat::detail::bulk_pull_receive_block_failure, nano::stat::dir::in);
		}
	});
}

void nano::bulk_pull_client::received_type ()
{
	auto this_l (shared_from_this ());
	nano::block_type type (static_cast<nano::block_type> (connection->receive_buffer->data ()[0]));
	switch (type)
	{
		case nano::block_type::send:
		{
			connection->channel->socket->async_read (connection->receive_buffer, nano::send_block::size, [this_l, type](boost::system::error_code const & ec, size_t size_a) {
				this_l->received_block (ec, size_a, type);
			});
			break;
		}
		case nano::block_type::receive:
		{
			connection->channel->socket->async_read (connection->receive_buffer, nano::receive_block::size, [this_l, type](boost::system::error_code const & ec, size_t size_a) {
				this_l->received_block (ec, size_a, type);
			});
			break;
		}
		case nano::block_type::open:
		{
			connection->channel->socket->async_read (connection->receive_buffer, nano::open_block::size, [this_l, type](boost::system::error_code const & ec, size_t size_a) {
				this_l->received_block (ec, size_a, type);
			});
			break;
		}
		case nano::block_type::change:
		{
			connection->channel->socket->async_read (connection->receive_buffer, nano::change_block::size, [this_l, type](boost::system::error_code const & ec, size_t size_a) {
				this_l->received_block (ec, size_a, type);
			});
			break;
		}
		case nano::block_type::state:
		{
			connection->channel->socket->async_read (connection->receive_buffer, nano::state_block::size, [this_l, type](boost::system::error_code const & ec, size_t size_a) {
				this_l->received_block (ec, size_a, type);
			});
			break;
		}
		case nano::block_type::not_a_block:
		{
			// Avoid re-using slow peers, or peers that sent the wrong blocks.
			if (!connection->pending_stop && expected == pull.end)
			{
				connection->attempt->pool_connection (connection);
			}
			break;
		}
		default:
		{
			if (connection->node->config.logging.network_packet_logging ())
			{
				connection->node->logger.try_log (boost::str (boost::format ("Unknown type received as block type: %1%") % static_cast<int> (type)));
			}
			break;
		}
	}
}

void nano::bulk_pull_client::received_block (boost::system::error_code const & ec, size_t size_a, nano::block_type type_a)
{
	if (!ec)
	{
		nano::bufferstream stream (connection->receive_buffer->data (), size_a);
		std::shared_ptr<nano::block> block (nano::deserialize_block (stream, type_a));
		if (block != nullptr && !nano::work_validate (*block))
		{
			auto hash (block->hash ());
			if (connection->node->config.logging.bulk_pull_logging ())
			{
				std::string block_l;
				block->serialize_json (block_l, connection->node->config.logging.single_line_record ());
				connection->node->logger.try_log (boost::str (boost::format ("Pulled block %1% %2%") % hash.to_string () % block_l));
			}
			// Is block expected?
			bool block_expected (false);
			if (hash == expected)
			{
				expected = block->previous ();
				block_expected = true;
			}
			else
			{
				unexpected_count++;
			}
			if (pull_blocks == 0 && block_expected)
			{
				known_account = block->account ();
			}
			if (connection->block_count++ == 0)
			{
				connection->start_time = std::chrono::steady_clock::now ();
			}
			connection->attempt->total_blocks++;
			bool stop_pull (connection->attempt->process_block (block, known_account, pull_blocks, block_expected));
			pull_blocks++;
			if (!stop_pull && !connection->hard_stop.load ())
			{
				/* Process block in lazy pull if not stopped
				Stop usual pull request with unexpected block & more than 16k blocks processed
				to prevent spam */
				if (connection->attempt->mode != nano::bootstrap_mode::legacy || unexpected_count < 16384)
				{
					throttled_receive_block ();
				}
			}
			else if (stop_pull && block_expected)
			{
				expected = pull.end;
				connection->attempt->pool_connection (connection);
			}
			if (stop_pull)
			{
				connection->attempt->lazy_stopped++;
			}
		}
		else
		{
			if (connection->node->config.logging.bulk_pull_logging ())
			{
				connection->node->logger.try_log ("Error deserializing block received from pull request");
			}
			connection->node->stats.inc (nano::stat::type::bootstrap, nano::stat::detail::bulk_pull_deserialize_receive_block, nano::stat::dir::in);
		}
	}
	else
	{
		if (connection->node->config.logging.bulk_pull_logging ())
		{
			connection->node->logger.try_log (boost::str (boost::format ("Error bulk receiving block: %1%") % ec.message ()));
		}
		connection->node->stats.inc (nano::stat::type::bootstrap, nano::stat::detail::bulk_pull_receive_block_failure, nano::stat::dir::in);
	}
}

nano::bulk_push_client::bulk_push_client (std::shared_ptr<nano::bootstrap_client> const & connection_a) :
connection (connection_a)
{
}

nano::bulk_push_client::~bulk_push_client ()
{
}

void nano::bulk_push_client::start ()
{
	nano::bulk_push message;
	auto this_l (shared_from_this ());
	connection->channel->send (
	message, [this_l](boost::system::error_code const & ec, size_t size_a) {
		auto transaction (this_l->connection->node->store.tx_begin_read ());
		if (!ec)
		{
			this_l->push (transaction);
		}
		else
		{
			if (this_l->connection->node->config.logging.bulk_pull_logging ())
			{
				this_l->connection->node->logger.try_log (boost::str (boost::format ("Unable to send bulk_push request: %1%") % ec.message ()));
			}
		}
	},
	false); // is bootstrap traffic is_droppable false
}

void nano::bulk_push_client::push (nano::transaction const & transaction_a)
{
	std::shared_ptr<nano::block> block;
	bool finished (false);
	while (block == nullptr && !finished)
	{
		if (current_target.first.is_zero () || current_target.first == current_target.second)
		{
			std::lock_guard<std::mutex> guard (connection->attempt->mutex);
			if (!connection->attempt->bulk_push_targets.empty ())
			{
				current_target = connection->attempt->bulk_push_targets.back ();
				connection->attempt->bulk_push_targets.pop_back ();
			}
			else
			{
				finished = true;
			}
		}
		if (!finished)
		{
			block = connection->node->store.block_get (transaction_a, current_target.first);
			if (block == nullptr)
			{
				current_target.first = nano::block_hash (0);
			}
			else
			{
				if (connection->node->config.logging.bulk_pull_logging ())
				{
					connection->node->logger.try_log ("Bulk pushing range ", current_target.first.to_string (), " down to ", current_target.second.to_string ());
				}
			}
		}
	}
	if (finished)
	{
		send_finished ();
	}
	else
	{
		current_target.first = block->previous ();
		push_block (*block);
	}
}

void nano::bulk_push_client::send_finished ()
{
	auto buffer (std::make_shared<std::vector<uint8_t>> ());
	buffer->push_back (static_cast<uint8_t> (nano::block_type::not_a_block));
	auto this_l (shared_from_this ());
	connection->channel->send_buffer (buffer, nano::stat::detail::all, [this_l](boost::system::error_code const & ec, size_t size_a) {
		try
		{
			this_l->promise.set_value (false);
		}
		catch (std::future_error &)
		{
		}
	});
}

void nano::bulk_push_client::push_block (nano::block const & block_a)
{
	auto buffer (std::make_shared<std::vector<uint8_t>> ());
	{
		nano::vectorstream stream (*buffer);
		nano::serialize_block (stream, block_a);
	}
	auto this_l (shared_from_this ());
	connection->channel->send_buffer (buffer, nano::stat::detail::all, [this_l](boost::system::error_code const & ec, size_t size_a) {
		if (!ec)
		{
			auto transaction (this_l->connection->node->store.tx_begin_read ());
			this_l->push (transaction);
		}
		else
		{
			if (this_l->connection->node->config.logging.bulk_pull_logging ())
			{
				this_l->connection->node->logger.try_log (boost::str (boost::format ("Error sending block during bulk push: %1%") % ec.message ()));
			}
		}
	});
}

nano::bulk_pull_account_client::bulk_pull_account_client (std::shared_ptr<nano::bootstrap_client> connection_a, nano::account const & account_a) :
connection (connection_a),
account (account_a),
pull_blocks (0)
{
	connection->attempt->condition.notify_all ();
}

nano::bulk_pull_account_client::~bulk_pull_account_client ()
{
	{
		std::lock_guard<std::mutex> mutex (connection->attempt->mutex);
		--connection->attempt->pulling;
	}
	connection->attempt->condition.notify_all ();
}

void nano::bulk_pull_account_client::request ()
{
	nano::bulk_pull_account req;
	req.account = account;
	req.minimum_amount = connection->node->config.receive_minimum;
	req.flags = nano::bulk_pull_account_flags::pending_hash_and_amount;
	if (connection->node->config.logging.bulk_pull_logging ())
	{
		std::unique_lock<std::mutex> lock (connection->attempt->mutex);
		connection->node->logger.try_log (boost::str (boost::format ("Requesting pending for account %1% from %2%. %3% accounts in queue") % req.account.to_account () % connection->channel->to_string () % connection->attempt->wallet_accounts.size ()));
	}
	else if (connection->node->config.logging.network_logging () && connection->attempt->should_log ())
	{
		std::unique_lock<std::mutex> lock (connection->attempt->mutex);
		connection->node->logger.always_log (boost::str (boost::format ("%1% accounts in pull queue") % connection->attempt->wallet_accounts.size ()));
	}
	auto this_l (shared_from_this ());
	connection->channel->send (
	req, [this_l](boost::system::error_code const & ec, size_t size_a) {
		if (!ec)
		{
			this_l->receive_pending ();
		}
		else
		{
			this_l->connection->attempt->requeue_pending (this_l->account);
			if (this_l->connection->node->config.logging.bulk_pull_logging ())
			{
				this_l->connection->node->logger.try_log (boost::str (boost::format ("Error starting bulk pull request to %1%: to %2%") % ec.message () % this_l->connection->channel->to_string ()));
			}
			this_l->connection->node->stats.inc (nano::stat::type::bootstrap, nano::stat::detail::bulk_pull_error_starting_request, nano::stat::dir::in);
		}
	},
	false); // is bootstrap traffic is_droppable false
}

void nano::bulk_pull_account_client::receive_pending ()
{
	auto this_l (shared_from_this ());
	size_t size_l (sizeof (nano::uint256_union) + sizeof (nano::uint128_union));
	connection->channel->socket->async_read (connection->receive_buffer, size_l, [this_l, size_l](boost::system::error_code const & ec, size_t size_a) {
		// An issue with asio is that sometimes, instead of reporting a bad file descriptor during disconnect,
		// we simply get a size of 0.
		if (size_a == size_l)
		{
			if (!ec)
			{
				nano::block_hash pending;
				nano::bufferstream frontier_stream (this_l->connection->receive_buffer->data (), sizeof (nano::uint256_union));
				auto error1 (nano::try_read (frontier_stream, pending));
				(void)error1;
				assert (!error1);
				nano::amount balance;
				nano::bufferstream balance_stream (this_l->connection->receive_buffer->data () + sizeof (nano::uint256_union), sizeof (nano::uint128_union));
				auto error2 (nano::try_read (balance_stream, balance));
				(void)error2;
				assert (!error2);
				if (this_l->pull_blocks == 0 || !pending.is_zero ())
				{
					if (this_l->pull_blocks == 0 || balance.number () >= this_l->connection->node->config.receive_minimum.number ())
					{
						this_l->pull_blocks++;
						{
							if (!pending.is_zero ())
							{
								auto transaction (this_l->connection->node->store.tx_begin_read ());
								if (!this_l->connection->node->store.block_exists (transaction, pending))
								{
									this_l->connection->attempt->lazy_start (pending);
								}
							}
						}
						this_l->receive_pending ();
					}
					else
					{
						this_l->connection->attempt->requeue_pending (this_l->account);
					}
				}
				else
				{
					this_l->connection->attempt->pool_connection (this_l->connection);
				}
			}
			else
			{
				this_l->connection->attempt->requeue_pending (this_l->account);
				if (this_l->connection->node->config.logging.network_logging ())
				{
					this_l->connection->node->logger.try_log (boost::str (boost::format ("Error while receiving bulk pull account frontier %1%") % ec.message ()));
				}
			}
		}
		else
		{
			this_l->connection->attempt->requeue_pending (this_l->account);
			if (this_l->connection->node->config.logging.network_message_logging ())
			{
				this_l->connection->node->logger.try_log (boost::str (boost::format ("Invalid size: expected %1%, got %2%") % size_l % size_a));
			}
		}
	});
}

nano::pull_info::pull_info (nano::account const & account_a, nano::block_hash const & head_a, nano::block_hash const & end_a, count_t count_a) :
account (account_a),
head (head_a),
head_original (head_a),
end (end_a),
count (count_a)
{
}

nano::bootstrap_attempt::bootstrap_attempt (std::shared_ptr<nano::node> node_a, nano::bootstrap_mode mode_a) :
next_log (std::chrono::steady_clock::now ()),
connections (0),
pulling (0),
node (node_a),
account_count (0),
total_blocks (0),
runs_count (0),
stopped (false),
mode (mode_a),
lazy_stopped (0)
{
	node->logger.always_log ("Starting bootstrap attempt");
	node->bootstrap_initiator.notify_listeners (true);
}

nano::bootstrap_attempt::~bootstrap_attempt ()
{
	node->logger.always_log ("Exiting bootstrap attempt");
	node->bootstrap_initiator.notify_listeners (false);
}

bool nano::bootstrap_attempt::should_log ()
{
	std::lock_guard<std::mutex> lock (mutex);
	auto result (false);
	auto now (std::chrono::steady_clock::now ());
	if (next_log < now)
	{
		result = true;
		next_log = now + std::chrono::seconds (15);
	}
	return result;
}

bool nano::bootstrap_attempt::request_frontier (std::unique_lock<std::mutex> & lock_a)
{
	auto result (true);
	auto connection_l (connection (lock_a));
	connection_frontier_request = connection_l;
	if (connection_l)
	{
		std::future<bool> future;
		{
			auto client (std::make_shared<nano::frontier_req_client> (connection_l));
			client->run ();
			frontiers = client;
			future = client->promise.get_future ();
		}
		lock_a.unlock ();
		result = consume_future (future); // This is out of scope of `client' so when the last reference via boost::asio::io_context is lost and the client is destroyed, the future throws an exception.
		lock_a.lock ();
		if (result)
		{
			pulls.clear ();
		}
		if (node->config.logging.network_logging ())
		{
			if (!result)
			{
				node->logger.try_log (boost::str (boost::format ("Completed frontier request, %1% out of sync accounts according to %2%") % pulls.size () % connection_l->channel->to_string ()));
			}
			else
			{
				node->stats.inc (nano::stat::type::error, nano::stat::detail::frontier_req, nano::stat::dir::out);
			}
		}
	}
	return result;
}

void nano::bootstrap_attempt::request_pull (std::unique_lock<std::mutex> & lock_a)
{
	auto connection_l (connection (lock_a));
	if (connection_l)
	{
		auto pull (pulls.front ());
		pulls.pop_front ();
		if (mode != nano::bootstrap_mode::legacy)
		{
			// Check if pull is obsolete (head was processed)
			std::unique_lock<std::mutex> lock (lazy_mutex);
			auto transaction (node->store.tx_begin_read ());
			while (!pulls.empty () && !pull.head.is_zero () && (lazy_blocks.find (pull.head) != lazy_blocks.end () || node->store.block_exists (transaction, pull.head)))
			{
				pull = pulls.front ();
				pulls.pop_front ();
			}
		}
		++pulling;
		// The bulk_pull_client destructor attempt to requeue_pull which can cause a deadlock if this is the last reference
		// Dispatch request in an external thread in case it needs to be destroyed
		node->background ([connection_l, pull]() {
			auto client (std::make_shared<nano::bulk_pull_client> (connection_l, pull));
			client->request ();
		});
	}
}

void nano::bootstrap_attempt::request_push (std::unique_lock<std::mutex> & lock_a)
{
	bool error (false);
	if (auto connection_shared = connection_frontier_request.lock ())
	{
		std::future<bool> future;
		{
			auto client (std::make_shared<nano::bulk_push_client> (connection_shared));
			client->start ();
			push = client;
			future = client->promise.get_future ();
		}
		lock_a.unlock ();
		error = consume_future (future); // This is out of scope of `client' so when the last reference via boost::asio::io_context is lost and the client is destroyed, the future throws an exception.
		lock_a.lock ();
	}
	if (node->config.logging.network_logging ())
	{
		node->logger.try_log ("Exiting bulk push client");
		if (error)
		{
			node->logger.try_log ("Bulk push client failed");
		}
	}
}

bool nano::bootstrap_attempt::still_pulling ()
{
	assert (!mutex.try_lock ());
	auto running (!stopped);
	auto more_pulls (!pulls.empty ());
	auto still_pulling (pulling > 0);
	return running && (more_pulls || still_pulling);
}

void nano::bootstrap_attempt::run ()
{
	assert (!node->flags.disable_legacy_bootstrap);
	populate_connections ();
	std::unique_lock<std::mutex> lock (mutex);
	auto frontier_failure (true);
	while (!stopped && frontier_failure)
	{
		frontier_failure = request_frontier (lock);
	}
	// Shuffle pulls.
	release_assert (std::numeric_limits<CryptoPP::word32>::max () > pulls.size ());
	if (!pulls.empty ())
	{
		for (auto i = static_cast<CryptoPP::word32> (pulls.size () - 1); i > 0; --i)
		{
			auto k = nano::random_pool::generate_word32 (0, i);
			std::swap (pulls[i], pulls[k]);
		}
	}
	while (still_pulling ())
	{
		while (still_pulling ())
		{
			if (!pulls.empty ())
			{
				request_pull (lock);
			}
			else
			{
				condition.wait (lock);
			}
		}
		// Flushing may resolve forks which can add more pulls
		node->logger.try_log ("Flushing unchecked blocks");
		lock.unlock ();
		node->block_processor.flush ();
		lock.lock ();
		node->logger.try_log ("Finished flushing unchecked blocks");
	}
	if (!stopped)
	{
		node->logger.try_log ("Completed pulls");
		request_push (lock);
		runs_count++;
		// Start wallet lazy bootstrap if required
		if (!wallet_accounts.empty () && !node->flags.disable_wallet_bootstrap)
		{
			lock.unlock ();
			mode = nano::bootstrap_mode::wallet_lazy;
			wallet_run ();
			lock.lock ();
		}
		// Start lazy bootstrap if some lazy keys were inserted
		else if (runs_count < 3 && !lazy_finished () && !node->flags.disable_lazy_bootstrap)
		{
			lock.unlock ();
			mode = nano::bootstrap_mode::lazy;
			lazy_run ();
			lock.lock ();
		}
		if (!node->flags.disable_unchecked_cleanup)
		{
			node->unchecked_cleanup ();
		}
	}
	stopped = true;
	condition.notify_all ();
	idle.clear ();
}

std::shared_ptr<nano::bootstrap_client> nano::bootstrap_attempt::connection (std::unique_lock<std::mutex> & lock_a)
{
	while (!stopped && idle.empty ())
	{
		condition.wait (lock_a);
	}
	std::shared_ptr<nano::bootstrap_client> result;
	if (!idle.empty ())
	{
		result = idle.back ();
		idle.pop_back ();
	}
	return result;
}

bool nano::bootstrap_attempt::consume_future (std::future<bool> & future_a)
{
	bool result;
	try
	{
		result = future_a.get ();
	}
	catch (std::future_error &)
	{
		result = true;
	}
	return result;
}

struct block_rate_cmp
{
	bool operator() (const std::shared_ptr<nano::bootstrap_client> & lhs, const std::shared_ptr<nano::bootstrap_client> & rhs) const
	{
		return lhs->block_rate () > rhs->block_rate ();
	}
};

unsigned nano::bootstrap_attempt::target_connections (size_t pulls_remaining)
{
	if (node->config.bootstrap_connections >= node->config.bootstrap_connections_max)
	{
		return std::max (1U, node->config.bootstrap_connections_max);
	}

	// Only scale up to bootstrap_connections_max for large pulls.
	double step = std::min (1.0, std::max (0.0, (double)pulls_remaining / bootstrap_connection_scale_target_blocks));
	double target = (double)node->config.bootstrap_connections + (double)(node->config.bootstrap_connections_max - node->config.bootstrap_connections) * step;
	return std::max (1U, (unsigned)(target + 0.5f));
}

void nano::bootstrap_attempt::populate_connections ()
{
	double rate_sum = 0.0;
	size_t num_pulls = 0;
	std::priority_queue<std::shared_ptr<nano::bootstrap_client>, std::vector<std::shared_ptr<nano::bootstrap_client>>, block_rate_cmp> sorted_connections;
	std::unordered_set<nano::tcp_endpoint> endpoints;
	{
		std::unique_lock<std::mutex> lock (mutex);
		num_pulls = pulls.size ();
		std::deque<std::weak_ptr<nano::bootstrap_client>> new_clients;
		for (auto & c : clients)
		{
			if (auto client = c.lock ())
			{
				new_clients.push_back (client);
				endpoints.insert (client->channel->socket->remote_endpoint ());
				double elapsed_sec = client->elapsed_seconds ();
				auto blocks_per_sec = client->block_rate ();
				rate_sum += blocks_per_sec;
				if (client->elapsed_seconds () > bootstrap_connection_warmup_time_sec && client->block_count > 0)
				{
					sorted_connections.push (client);
				}
				// Force-stop the slowest peers, since they can take the whole bootstrap hostage by dribbling out blocks on the last remaining pull.
				// This is ~1.5kilobits/sec.
				if (elapsed_sec > bootstrap_minimum_termination_time_sec && blocks_per_sec < bootstrap_minimum_blocks_per_sec)
				{
					if (node->config.logging.bulk_pull_logging ())
					{
						node->logger.try_log (boost::str (boost::format ("Stopping slow peer %1% (elapsed sec %2%s > %3%s and %4% blocks per second < %5%)") % client->channel->to_string () % elapsed_sec % bootstrap_minimum_termination_time_sec % blocks_per_sec % bootstrap_minimum_blocks_per_sec));
					}

					client->stop (true);
				}
			}
		}
		// Cleanup expired clients
		clients.swap (new_clients);
	}

	auto target = target_connections (num_pulls);

	// We only want to drop slow peers when more than 2/3 are active. 2/3 because 1/2 is too aggressive, and 100% rarely happens.
	// Probably needs more tuning.
	if (sorted_connections.size () >= (target * 2) / 3 && target >= 4)
	{
		// 4 -> 1, 8 -> 2, 16 -> 4, arbitrary, but seems to work well.
		auto drop = (int)roundf (sqrtf ((float)target - 2.0f));

		if (node->config.logging.bulk_pull_logging ())
		{
			node->logger.try_log (boost::str (boost::format ("Dropping %1% bulk pull peers, target connections %2%") % drop % target));
		}

		for (int i = 0; i < drop; i++)
		{
			auto client = sorted_connections.top ();

			if (node->config.logging.bulk_pull_logging ())
			{
				node->logger.try_log (boost::str (boost::format ("Dropping peer with block rate %1%, block count %2% (%3%) ") % client->block_rate () % client->block_count % client->channel->to_string ()));
			}

			client->stop (false);
			sorted_connections.pop ();
		}
	}

	if (node->config.logging.bulk_pull_logging ())
	{
		std::unique_lock<std::mutex> lock (mutex);
		node->logger.try_log (boost::str (boost::format ("Bulk pull connections: %1%, rate: %2% blocks/sec, remaining account pulls: %3%, total blocks: %4%") % connections.load () % (int)rate_sum % pulls.size () % (int)total_blocks.load ()));
	}

	if (connections < target)
	{
		auto delta = std::min ((target - connections) * 2, bootstrap_max_new_connections);
		// TODO - tune this better
		// Not many peers respond, need to try to make more connections than we need.
		for (auto i = 0u; i < delta; i++)
		{
			auto endpoint (node->network.bootstrap_peer ());
			if (endpoint != nano::tcp_endpoint (boost::asio::ip::address_v6::any (), 0) && endpoints.find (endpoint) == endpoints.end ())
			{
				connect_client (endpoint);
				std::lock_guard<std::mutex> lock (mutex);
				endpoints.insert (endpoint);
			}
			else if (connections == 0)
			{
				node->logger.try_log (boost::str (boost::format ("Bootstrap stopped because there are no peers")));
				stopped = true;
				condition.notify_all ();
			}
		}
	}
	if (!stopped)
	{
		std::weak_ptr<nano::bootstrap_attempt> this_w (shared_from_this ());
		node->alarm.add (std::chrono::steady_clock::now () + std::chrono::seconds (1), [this_w]() {
			if (auto this_l = this_w.lock ())
			{
				this_l->populate_connections ();
			}
		});
	}
}

void nano::bootstrap_attempt::add_connection (nano::endpoint const & endpoint_a)
{
	connect_client (nano::tcp_endpoint (endpoint_a.address (), endpoint_a.port ()));
}

void nano::bootstrap_attempt::connect_client (nano::tcp_endpoint const & endpoint_a)
{
	++connections;
	auto socket (std::make_shared<nano::socket> (node));
	auto this_l (shared_from_this ());
	socket->async_connect (endpoint_a,
	[this_l, socket, endpoint_a](boost::system::error_code const & ec) {
		if (!ec)
		{
			if (this_l->node->config.logging.bulk_pull_logging ())
			{
				this_l->node->logger.try_log (boost::str (boost::format ("Connection established to %1%") % endpoint_a));
			}
			auto client (std::make_shared<nano::bootstrap_client> (this_l->node, this_l, std::make_shared<nano::transport::channel_tcp> (*this_l->node, socket)));
			this_l->pool_connection (client);
		}
		else
		{
			if (this_l->node->config.logging.network_logging ())
			{
				switch (ec.value ())
				{
					default:
						this_l->node->logger.try_log (boost::str (boost::format ("Error initiating bootstrap connection to %1%: %2%") % endpoint_a % ec.message ()));
						break;
					case boost::system::errc::connection_refused:
					case boost::system::errc::operation_canceled:
					case boost::system::errc::timed_out:
					case 995: //Windows The I/O operation has been aborted because of either a thread exit or an application request
					case 10061: //Windows No connection could be made because the target machine actively refused it
						break;
				}
			}
		}
		--this_l->connections;
	});
}

void nano::bootstrap_attempt::pool_connection (std::shared_ptr<nano::bootstrap_client> client_a)
{
	std::lock_guard<std::mutex> lock (mutex);
	if (!stopped && !client_a->pending_stop)
	{
		// Idle bootstrap client socket
		client_a->channel->socket->start_timer (node->network_params.node.idle_timeout);
		// Push into idle deque
		idle.push_front (client_a);
	}
	condition.notify_all ();
}

void nano::bootstrap_attempt::stop ()
{
	std::lock_guard<std::mutex> lock (mutex);
	stopped = true;
	condition.notify_all ();
	for (auto i : clients)
	{
		if (auto client = i.lock ())
		{
			client->channel->socket->close ();
		}
	}
	if (auto i = frontiers.lock ())
	{
		try
		{
			i->promise.set_value (true);
		}
		catch (std::future_error &)
		{
		}
	}
	if (auto i = push.lock ())
	{
		try
		{
			i->promise.set_value (true);
		}
		catch (std::future_error &)
		{
		}
	}
}

void nano::bootstrap_attempt::add_pull (nano::pull_info const & pull_a)
{
	nano::pull_info pull (pull_a);
	node->bootstrap_initiator.cache.update_pull (pull);
	{
		std::lock_guard<std::mutex> lock (mutex);
		pulls.push_back (pull);
	}
	condition.notify_all ();
}

void nano::bootstrap_attempt::requeue_pull (nano::pull_info const & pull_a)
{
	auto pull (pull_a);
	if (++pull.attempts < (bootstrap_frontier_retry_limit + (pull.processed / 10000)))
	{
		std::lock_guard<std::mutex> lock (mutex);
		pulls.push_front (pull);
		condition.notify_all ();
	}
	else if (mode == nano::bootstrap_mode::lazy)
	{
		{
			// Retry for lazy pulls (not weak state block link assumptions)
			std::lock_guard<std::mutex> lock (mutex);
			pull.attempts++;
			pulls.push_back (pull);
		}
		condition.notify_all ();
	}
	else
	{
		if (node->config.logging.bulk_pull_logging ())
		{
			node->logger.try_log (boost::str (boost::format ("Failed to pull account %1% down to %2% after %3% attempts and %4% blocks processed") % pull.account.to_account () % pull.end.to_string () % pull.attempts % pull.processed));
		}
		node->stats.inc (nano::stat::type::bootstrap, nano::stat::detail::bulk_pull_failed_account, nano::stat::dir::in);

		node->bootstrap_initiator.cache.add (pull);
	}
}

void nano::bootstrap_attempt::add_bulk_push_target (nano::block_hash const & head, nano::block_hash const & end)
{
	std::lock_guard<std::mutex> lock (mutex);
	bulk_push_targets.push_back (std::make_pair (head, end));
}

void nano::bootstrap_attempt::lazy_start (nano::block_hash const & hash_a)
{
	std::unique_lock<std::mutex> lock (lazy_mutex);
	// Add start blocks, limit 1024 (32k with disabled legacy bootstrap)
	size_t max_keys (node->flags.disable_legacy_bootstrap ? 32 * 1024 : 1024);
	if (lazy_keys.size () < max_keys && lazy_keys.find (hash_a) == lazy_keys.end () && lazy_blocks.find (hash_a) == lazy_blocks.end ())
	{
		lazy_keys.insert (hash_a);
		lazy_pulls.push_back (hash_a);
	}
}

void nano::bootstrap_attempt::lazy_add (nano::block_hash const & hash_a)
{
	// Add only unknown blocks
	assert (!lazy_mutex.try_lock ());
	if (lazy_blocks.find (hash_a) == lazy_blocks.end ())
	{
		lazy_pulls.push_back (hash_a);
	}
}

void nano::bootstrap_attempt::lazy_pull_flush ()
{
	assert (!mutex.try_lock ());
	std::unique_lock<std::mutex> lazy_lock (lazy_mutex);
	auto transaction (node->store.tx_begin_read ());
	for (auto & pull_start : lazy_pulls)
	{
		// Recheck if block was already processed
		if (lazy_blocks.find (pull_start) == lazy_blocks.end () && !node->store.block_exists (transaction, pull_start))
		{
			assert (node->network_params.bootstrap.lazy_max_pull_blocks <= std::numeric_limits<nano::pull_info::count_t>::max ());
			pulls.push_back (nano::pull_info (pull_start, pull_start, nano::block_hash (0), static_cast<nano::pull_info::count_t> (node->network_params.bootstrap.lazy_max_pull_blocks)));
		}
	}
	lazy_pulls.clear ();
}

bool nano::bootstrap_attempt::lazy_finished ()
{
	bool result (true);
	auto transaction (node->store.tx_begin_read ());
	std::unique_lock<std::mutex> lock (lazy_mutex);
	for (auto it (lazy_keys.begin ()), end (lazy_keys.end ()); it != end && !stopped;)
	{
		if (node->store.block_exists (transaction, *it))
		{
			it = lazy_keys.erase (it);
		}
		else
		{
			result = false;
			break;
			// No need to increment `it` as we break above.
		}
	}
	// Finish lazy bootstrap without lazy pulls (in combination with still_pulling ())
	if (!result && lazy_pulls.empty ())
	{
		result = true;
	}
	return result;
}

void nano::bootstrap_attempt::lazy_clear ()
{
	assert (!lazy_mutex.try_lock ());
	lazy_blocks.clear ();
	lazy_keys.clear ();
	lazy_pulls.clear ();
	lazy_state_unknown.clear ();
	lazy_balances.clear ();
	lazy_stopped = 0;
}

void nano::bootstrap_attempt::lazy_run ()
{
	assert (!node->flags.disable_lazy_bootstrap);
	populate_connections ();
	auto start_time (std::chrono::steady_clock::now ());
	auto max_time (std::chrono::minutes (node->flags.disable_legacy_bootstrap ? 48 * 60 : 30));
	std::unique_lock<std::mutex> lock (mutex);
	while ((still_pulling () || !lazy_finished ()) && lazy_stopped < lazy_max_stopped && std::chrono::steady_clock::now () - start_time < max_time)
	{
		unsigned iterations (0);
		while (still_pulling () && lazy_stopped < lazy_max_stopped && std::chrono::steady_clock::now () - start_time < max_time)
		{
			if (!pulls.empty ())
			{
				request_pull (lock);
			}
			else
			{
				condition.wait (lock);
			}
			++iterations;
			// Flushing lazy pulls
			if (iterations % 100 == 0)
			{
				lazy_pull_flush ();
			}
		}
		// Flushing may resolve forks which can add more pulls
		// Flushing lazy pulls
		lock.unlock ();
		node->block_processor.flush ();
		lock.lock ();
		lazy_pull_flush ();
	}
	if (!stopped)
	{
		node->logger.try_log ("Completed lazy pulls");
		std::unique_lock<std::mutex> lazy_lock (lazy_mutex);
		runs_count++;
		// Start wallet lazy bootstrap if required
		if (!wallet_accounts.empty () && !node->flags.disable_wallet_bootstrap)
		{
			pulls.clear ();
			lazy_clear ();
			mode = nano::bootstrap_mode::wallet_lazy;
			lock.unlock ();
			lazy_lock.unlock ();
			wallet_run ();
			lock.lock ();
		}
		// Fallback to legacy bootstrap
		else if (runs_count < 3 && !lazy_keys.empty () && !node->flags.disable_legacy_bootstrap)
		{
			pulls.clear ();
			lazy_clear ();
			mode = nano::bootstrap_mode::legacy;
			lock.unlock ();
			lazy_lock.unlock ();
			run ();
			lock.lock ();
		}
	}
	stopped = true;
	condition.notify_all ();
	idle.clear ();
}

bool nano::bootstrap_attempt::process_block (std::shared_ptr<nano::block> block_a, nano::account const & known_account_a, uint64_t pull_blocks, bool block_expected)
{
	bool stop_pull (false);
	if (mode != nano::bootstrap_mode::legacy && block_expected)
	{
		auto hash (block_a->hash ());
		std::unique_lock<std::mutex> lock (lazy_mutex);
		// Processing new blocks
		if (lazy_blocks.find (hash) == lazy_blocks.end ())
		{
			// Search block in ledger (old)
			auto transaction (node->store.tx_begin_read ());
			if (!node->store.block_exists (transaction, block_a->type (), hash))
			{
				nano::uint128_t balance (std::numeric_limits<nano::uint128_t>::max ());
				nano::unchecked_info info (block_a, known_account_a, 0, nano::signature_verification::unknown);
				node->block_processor.add (info);
				// Search for new dependencies
				if (!block_a->source ().is_zero () && !node->store.block_exists (transaction, block_a->source ()))
				{
					lazy_add (block_a->source ());
				}
				else if (block_a->type () == nano::block_type::send)
				{
					// Calculate balance for legacy send blocks
					std::shared_ptr<nano::send_block> block_l (std::static_pointer_cast<nano::send_block> (block_a));
					if (block_l != nullptr)
					{
						balance = block_l->hashables.balance.number ();
					}
				}
				else if (block_a->type () == nano::block_type::state)
				{
					std::shared_ptr<nano::state_block> block_l (std::static_pointer_cast<nano::state_block> (block_a));
					if (block_l != nullptr)
					{
						balance = block_l->hashables.balance.number ();
						nano::block_hash link (block_l->hashables.link);
						// If link is not epoch link or 0. And if block from link unknown
						if (!link.is_zero () && link != node->ledger.epoch_link && lazy_blocks.find (link) == lazy_blocks.end () && !node->store.block_exists (transaction, link))
						{
							nano::block_hash previous (block_l->hashables.previous);
							// If state block previous is 0 then source block required
							if (previous.is_zero ())
							{
								lazy_add (link);
							}
							// In other cases previous block balance required to find out subtype of state block
							else if (node->store.block_exists (transaction, previous))
							{
								nano::amount prev_balance (node->ledger.balance (transaction, previous));
								if (prev_balance.number () <= balance)
								{
									lazy_add (link);
								}
							}
							// Search balance of already processed previous blocks
							else if (lazy_blocks.find (previous) != lazy_blocks.end ())
							{
								auto previous_balance (lazy_balances.find (previous));
								if (previous_balance != lazy_balances.end ())
								{
									if (previous_balance->second <= balance)
									{
										lazy_add (link);
									}
									lazy_balances.erase (previous_balance);
								}
							}
							// Insert in unknown state blocks if previous wasn't already processed
							else
							{
								lazy_state_unknown.insert (std::make_pair (previous, std::make_pair (link, balance)));
							}
						}
					}
				}
				lazy_blocks.insert (hash);
				// Adding lazy balances
				if (pull_blocks == 0)
				{
					lazy_balances.insert (std::make_pair (hash, balance));
				}
				// Removing lazy balances
				if (!block_a->previous ().is_zero () && lazy_balances.find (block_a->previous ()) != lazy_balances.end ())
				{
					lazy_balances.erase (block_a->previous ());
				}
			}
			// Drop bulk_pull if block is already known (ledger)
			else
			{
				// Disabled until server rewrite
				// stop_pull = true;
				// Force drop lazy bootstrap connection for long bulk_pull
				if (pull_blocks > node->network_params.bootstrap.lazy_max_pull_blocks)
				{
					stop_pull = true;
				}
			}
			//Search unknown state blocks balances
			auto find_state (lazy_state_unknown.find (hash));
			if (find_state != lazy_state_unknown.end ())
			{
				auto next_block (find_state->second);
				lazy_state_unknown.erase (hash);
				// Retrieve balance for previous state blocks
				if (block_a->type () == nano::block_type::state)
				{
					std::shared_ptr<nano::state_block> block_l (std::static_pointer_cast<nano::state_block> (block_a));
					if (block_l->hashables.balance.number () <= next_block.second)
					{
						lazy_add (next_block.first);
					}
				}
				// Retrieve balance for previous legacy send blocks
				else if (block_a->type () == nano::block_type::send)
				{
					std::shared_ptr<nano::send_block> block_l (std::static_pointer_cast<nano::send_block> (block_a));
					if (block_l->hashables.balance.number () <= next_block.second)
					{
						lazy_add (next_block.first);
					}
				}
				// Weak assumption for other legacy block types
				else
				{
					// Disabled
				}
			}
		}
		// Drop bulk_pull if block is already known (processed set)
		else
		{
			// Disabled until server rewrite
			// stop_pull = true;
			// Force drop lazy bootstrap connection for long bulk_pull
			if (pull_blocks > node->network_params.bootstrap.lazy_max_pull_blocks)
			{
				stop_pull = true;
			}
		}
	}
	else if (mode != nano::bootstrap_mode::legacy)
	{
		// Drop connection with unexpected block for lazy bootstrap
		stop_pull = true;
	}
	else
	{
		nano::unchecked_info info (block_a, known_account_a, 0, nano::signature_verification::unknown);
		node->block_processor.add (info);
	}
	return stop_pull;
}

void nano::bootstrap_attempt::request_pending (std::unique_lock<std::mutex> & lock_a)
{
	auto connection_l (connection (lock_a));
	if (connection_l)
	{
		auto account (wallet_accounts.front ());
		wallet_accounts.pop_front ();
		++pulling;
		// The bulk_pull_account_client destructor attempt to requeue_pull which can cause a deadlock if this is the last reference
		// Dispatch request in an external thread in case it needs to be destroyed
		node->background ([connection_l, account]() {
			auto client (std::make_shared<nano::bulk_pull_account_client> (connection_l, account));
			client->request ();
		});
	}
}

void nano::bootstrap_attempt::requeue_pending (nano::account const & account_a)
{
	auto account (account_a);
	{
		std::lock_guard<std::mutex> lock (mutex);
		wallet_accounts.push_front (account);
		condition.notify_all ();
	}
}

void nano::bootstrap_attempt::wallet_start (std::deque<nano::account> & accounts_a)
{
	std::lock_guard<std::mutex> lock (mutex);
	wallet_accounts.swap (accounts_a);
}

bool nano::bootstrap_attempt::wallet_finished ()
{
	assert (!mutex.try_lock ());
	auto running (!stopped);
	auto more_accounts (!wallet_accounts.empty ());
	auto still_pulling (pulling > 0);
	return running && (more_accounts || still_pulling);
}

void nano::bootstrap_attempt::wallet_run ()
{
	assert (!node->flags.disable_wallet_bootstrap);
	populate_connections ();
	auto start_time (std::chrono::steady_clock::now ());
	auto max_time (std::chrono::minutes (10));
	std::unique_lock<std::mutex> lock (mutex);
	while (wallet_finished () && std::chrono::steady_clock::now () - start_time < max_time)
	{
		if (!wallet_accounts.empty ())
		{
			request_pending (lock);
		}
		else
		{
			condition.wait (lock);
		}
	}
	if (!stopped)
	{
		node->logger.try_log ("Completed wallet lazy pulls");
		runs_count++;
		// Start lazy bootstrap if some lazy keys were inserted
		if (!lazy_finished ())
		{
			lock.unlock ();
			lazy_run ();
			lock.lock ();
		}
	}
	stopped = true;
	condition.notify_all ();
	idle.clear ();
}

nano::bootstrap_initiator::bootstrap_initiator (nano::node & node_a) :
node (node_a),
stopped (false),
thread ([this]() {
	nano::thread_role::set (nano::thread_role::name::bootstrap_initiator);
	run_bootstrap ();
})
{
}

nano::bootstrap_initiator::~bootstrap_initiator ()
{
	stop ();
}

void nano::bootstrap_initiator::bootstrap ()
{
	std::unique_lock<std::mutex> lock (mutex);
	if (!stopped && attempt == nullptr)
	{
		node.stats.inc (nano::stat::type::bootstrap, nano::stat::detail::initiate, nano::stat::dir::out);
		attempt = std::make_shared<nano::bootstrap_attempt> (node.shared ());
		condition.notify_all ();
	}
}

void nano::bootstrap_initiator::bootstrap (nano::endpoint const & endpoint_a, bool add_to_peers)
{
	if (add_to_peers)
	{
		node.network.udp_channels.insert (nano::transport::map_endpoint_to_v6 (endpoint_a), node.network_params.protocol.protocol_version);
	}
	std::unique_lock<std::mutex> lock (mutex);
	if (!stopped)
	{
		while (attempt != nullptr)
		{
			attempt->stop ();
			condition.wait (lock);
		}
		node.stats.inc (nano::stat::type::bootstrap, nano::stat::detail::initiate, nano::stat::dir::out);
		attempt = std::make_shared<nano::bootstrap_attempt> (node.shared ());
		attempt->add_connection (endpoint_a);
		condition.notify_all ();
	}
}

void nano::bootstrap_initiator::bootstrap_lazy (nano::block_hash const & hash_a, bool force)
{
	{
		std::unique_lock<std::mutex> lock (mutex);
		if (force)
		{
			while (attempt != nullptr)
			{
				attempt->stop ();
				condition.wait (lock);
			}
		}
		node.stats.inc (nano::stat::type::bootstrap, nano::stat::detail::initiate_lazy, nano::stat::dir::out);
		if (attempt == nullptr)
		{
			attempt = std::make_shared<nano::bootstrap_attempt> (node.shared (), nano::bootstrap_mode::lazy);
		}
		attempt->lazy_start (hash_a);
	}
	condition.notify_all ();
}

void nano::bootstrap_initiator::bootstrap_wallet (std::deque<nano::account> & accounts_a)
{
	{
		std::unique_lock<std::mutex> lock (mutex);
		node.stats.inc (nano::stat::type::bootstrap, nano::stat::detail::initiate_wallet_lazy, nano::stat::dir::out);
		if (attempt == nullptr)
		{
			attempt = std::make_shared<nano::bootstrap_attempt> (node.shared (), nano::bootstrap_mode::wallet_lazy);
		}
		attempt->wallet_start (accounts_a);
	}
	condition.notify_all ();
}

void nano::bootstrap_initiator::run_bootstrap ()
{
	std::unique_lock<std::mutex> lock (mutex);
	while (!stopped)
	{
		if (attempt != nullptr)
		{
			lock.unlock ();
			if (attempt->mode == nano::bootstrap_mode::legacy)
			{
				attempt->run ();
			}
			else if (attempt->mode == nano::bootstrap_mode::lazy)
			{
				attempt->lazy_run ();
			}
			else
			{
				attempt->wallet_run ();
			}
			lock.lock ();
			attempt = nullptr;
			condition.notify_all ();
		}
		else
		{
			condition.wait (lock);
		}
	}
}

void nano::bootstrap_initiator::add_observer (std::function<void(bool)> const & observer_a)
{
	std::lock_guard<std::mutex> lock (observers_mutex);
	observers.push_back (observer_a);
}

bool nano::bootstrap_initiator::in_progress ()
{
	return current_attempt () != nullptr;
}

std::shared_ptr<nano::bootstrap_attempt> nano::bootstrap_initiator::current_attempt ()
{
	std::lock_guard<std::mutex> lock (mutex);
	return attempt;
}

void nano::bootstrap_initiator::stop ()
{
	if (!stopped.exchange (true))
	{
		{
			std::lock_guard<std::mutex> guard (mutex);
			if (attempt != nullptr)
			{
				attempt->stop ();
			}
		}
		condition.notify_all ();

		if (thread.joinable ())
		{
			thread.join ();
		}
	}
}

void nano::bootstrap_initiator::notify_listeners (bool in_progress_a)
{
	std::lock_guard<std::mutex> lock (observers_mutex);
	for (auto & i : observers)
	{
		i (in_progress_a);
	}
}

namespace nano
{
std::unique_ptr<seq_con_info_component> collect_seq_con_info (bootstrap_initiator & bootstrap_initiator, const std::string & name)
{
	size_t count = 0;
	size_t cache_count = 0;
	{
		std::lock_guard<std::mutex> guard (bootstrap_initiator.observers_mutex);
		count = bootstrap_initiator.observers.size ();
	}
	{
		std::lock_guard<std::mutex> guard (bootstrap_initiator.cache.pulls_cache_mutex);
		cache_count = bootstrap_initiator.cache.cache.size ();
	}

	auto sizeof_element = sizeof (decltype (bootstrap_initiator.observers)::value_type);
	auto sizeof_cache_element = sizeof (decltype (bootstrap_initiator.cache.cache)::value_type);
	auto composite = std::make_unique<seq_con_info_composite> (name);
	composite->add_component (std::make_unique<seq_con_info_leaf> (seq_con_info{ "observers", count, sizeof_element }));
	composite->add_component (std::make_unique<seq_con_info_leaf> (seq_con_info{ "pulls_cache", cache_count, sizeof_cache_element }));
	return composite;
}
}

nano::bootstrap_listener::bootstrap_listener (uint16_t port_a, nano::node & node_a) :
node (node_a),
port (port_a)
{
}

void nano::bootstrap_listener::start ()
{
	listening_socket = std::make_shared<nano::server_socket> (node.shared (), boost::asio::ip::tcp::endpoint (boost::asio::ip::address_v6::any (), port), node.config.tcp_incoming_connections_max);
	boost::system::error_code ec;
	listening_socket->start (ec);
	if (ec)
	{
		node.logger.try_log (boost::str (boost::format ("Error while binding for incoming TCP/bootstrap on port %1%: %2%") % listening_socket->listening_port () % ec.message ()));
		throw std::runtime_error (ec.message ());
	}
	listening_socket->on_connection ([this](std::shared_ptr<nano::socket> new_connection, boost::system::error_code const & ec_a) {
		bool keep_accepting = true;
		if (ec_a)
		{
			keep_accepting = false;
			this->node.logger.try_log (boost::str (boost::format ("Error while accepting incoming TCP/bootstrap connections: %1%") % ec_a.message ()));
		}
		else
		{
			accept_action (ec_a, new_connection);
		}
		return keep_accepting;
	});
}

void nano::bootstrap_listener::stop ()
{
	decltype (connections) connections_l;
	{
		std::lock_guard<std::mutex> lock (mutex);
		on = false;
		connections_l.swap (connections);
	}
	if (listening_socket)
	{
		listening_socket->close ();
		listening_socket = nullptr;
	}
}

size_t nano::bootstrap_listener::connection_count ()
{
	std::lock_guard<std::mutex> lock (mutex);
	return connections.size ();
}

void nano::bootstrap_listener::accept_action (boost::system::error_code const & ec, std::shared_ptr<nano::socket> socket_a)
{
	auto connection (std::make_shared<nano::bootstrap_server> (socket_a, node.shared ()));
	{
		std::lock_guard<std::mutex> lock (mutex);
		connections[connection.get ()] = connection;
		connection->receive ();
	}
}

boost::asio::ip::tcp::endpoint nano::bootstrap_listener::endpoint ()
{
	return boost::asio::ip::tcp::endpoint (boost::asio::ip::address_v6::loopback (), listening_socket->listening_port ());
}

namespace nano
{
std::unique_ptr<seq_con_info_component> collect_seq_con_info (bootstrap_listener & bootstrap_listener, const std::string & name)
{
	auto sizeof_element = sizeof (decltype (bootstrap_listener.connections)::value_type);
	auto composite = std::make_unique<seq_con_info_composite> (name);
	composite->add_component (std::make_unique<seq_con_info_leaf> (seq_con_info{ "connections", bootstrap_listener.connection_count (), sizeof_element }));
	return composite;
}
}

nano::bootstrap_server::~bootstrap_server ()
{
	if (node->config.logging.bulk_pull_logging ())
	{
		node->logger.try_log ("Exiting incoming TCP/bootstrap server");
	}
	if (type == nano::bootstrap_server_type::bootstrap)
	{
		--node->bootstrap.bootstrap_count;
	}
	else if (type == nano::bootstrap_server_type::realtime)
	{
		--node->bootstrap.realtime_count;
		node->network.response_channels.remove (remote_endpoint);
		// Clear temporary channel
		auto exisiting_response_channel (node->network.tcp_channels.find_channel (remote_endpoint));
		if (exisiting_response_channel != nullptr)
		{
			exisiting_response_channel->server = false;
			node->network.tcp_channels.erase (remote_endpoint);
		}
	}
	stop ();
	std::lock_guard<std::mutex> lock (node->bootstrap.mutex);
	node->bootstrap.connections.erase (this);
}

void nano::bootstrap_server::stop ()
{
	if (!stopped.exchange (true))
	{
		if (socket != nullptr)
		{
			socket->close ();
		}
	}
}

nano::bootstrap_server::bootstrap_server (std::shared_ptr<nano::socket> socket_a, std::shared_ptr<nano::node> node_a) :
receive_buffer (std::make_shared<std::vector<uint8_t>> ()),
socket (socket_a),
node (node_a)
{
	receive_buffer->resize (512);
}

void nano::bootstrap_server::receive ()
{
	// Increase timeout to receive TCP header (idle server socket)
	socket->set_timeout (node->network_params.node.idle_timeout);
	auto this_l (shared_from_this ());
	socket->async_read (receive_buffer, 8, [this_l](boost::system::error_code const & ec, size_t size_a) {
		// Set remote_endpoint
		if (this_l->remote_endpoint.port () == 0)
		{
			this_l->remote_endpoint = this_l->socket->remote_endpoint ();
		}
		// Decrease timeout to default
		this_l->socket->set_timeout (this_l->node->config.tcp_io_timeout);
		// Receive header
		this_l->receive_header_action (ec, size_a);
	});
}

void nano::bootstrap_server::receive_header_action (boost::system::error_code const & ec, size_t size_a)
{
	if (!ec)
	{
		assert (size_a == 8);
		nano::bufferstream type_stream (receive_buffer->data (), size_a);
		auto error (false);
		nano::message_header header (error, type_stream);
		if (!error)
		{
			switch (header.type)
			{
				case nano::message_type::bulk_pull:
				{
					node->stats.inc (nano::stat::type::bootstrap, nano::stat::detail::bulk_pull, nano::stat::dir::in);
					auto this_l (shared_from_this ());
					socket->async_read (receive_buffer, header.payload_length_bytes (), [this_l, header](boost::system::error_code const & ec, size_t size_a) {
						this_l->receive_bulk_pull_action (ec, size_a, header);
					});
					break;
				}
				case nano::message_type::bulk_pull_account:
				{
					node->stats.inc (nano::stat::type::bootstrap, nano::stat::detail::bulk_pull_account, nano::stat::dir::in);
					auto this_l (shared_from_this ());
					socket->async_read (receive_buffer, header.payload_length_bytes (), [this_l, header](boost::system::error_code const & ec, size_t size_a) {
						this_l->receive_bulk_pull_account_action (ec, size_a, header);
					});
					break;
				}
				case nano::message_type::frontier_req:
				{
					node->stats.inc (nano::stat::type::bootstrap, nano::stat::detail::frontier_req, nano::stat::dir::in);
					auto this_l (shared_from_this ());
					socket->async_read (receive_buffer, header.payload_length_bytes (), [this_l, header](boost::system::error_code const & ec, size_t size_a) {
						this_l->receive_frontier_req_action (ec, size_a, header);
					});
					break;
				}
				case nano::message_type::bulk_push:
				{
					node->stats.inc (nano::stat::type::bootstrap, nano::stat::detail::bulk_push, nano::stat::dir::in);
					if (is_bootstrap_connection ())
					{
						add_request (std::unique_ptr<nano::message> (new nano::bulk_push (header)));
					}
					break;
				}
				case nano::message_type::keepalive:
				{
					auto this_l (shared_from_this ());
					socket->async_read (receive_buffer, header.payload_length_bytes (), [this_l, header](boost::system::error_code const & ec, size_t size_a) {
						this_l->receive_keepalive_action (ec, size_a, header);
					});
					break;
				}
				case nano::message_type::publish:
				{
					auto this_l (shared_from_this ());
					socket->async_read (receive_buffer, header.payload_length_bytes (), [this_l, header](boost::system::error_code const & ec, size_t size_a) {
						this_l->receive_publish_action (ec, size_a, header);
					});
					break;
				}
				case nano::message_type::confirm_ack:
				{
					auto this_l (shared_from_this ());
					socket->async_read (receive_buffer, header.payload_length_bytes (), [this_l, header](boost::system::error_code const & ec, size_t size_a) {
						this_l->receive_confirm_ack_action (ec, size_a, header);
					});
					break;
				}
				case nano::message_type::confirm_req:
				{
					auto this_l (shared_from_this ());
					socket->async_read (receive_buffer, header.payload_length_bytes (), [this_l, header](boost::system::error_code const & ec, size_t size_a) {
						this_l->receive_confirm_req_action (ec, size_a, header);
					});
					break;
				}
				case nano::message_type::node_id_handshake:
				{
					auto this_l (shared_from_this ());
					socket->async_read (receive_buffer, header.payload_length_bytes (), [this_l, header](boost::system::error_code const & ec, size_t size_a) {
						this_l->receive_node_id_handshake_action (ec, size_a, header);
					});
					break;
				}
				default:
				{
					if (node->config.logging.network_logging ())
					{
						node->logger.try_log (boost::str (boost::format ("Received invalid type from bootstrap connection %1%") % static_cast<uint8_t> (header.type)));
					}
					break;
				}
			}
		}
	}
	else
	{
		if (node->config.logging.bulk_pull_logging ())
		{
			node->logger.try_log (boost::str (boost::format ("Error while receiving type: %1%") % ec.message ()));
		}
	}
}

void nano::bootstrap_server::receive_bulk_pull_action (boost::system::error_code const & ec, size_t size_a, nano::message_header const & header_a)
{
	if (!ec)
	{
		auto error (false);
		nano::bufferstream stream (receive_buffer->data (), size_a);
		std::unique_ptr<nano::bulk_pull> request (new nano::bulk_pull (error, stream, header_a));
		if (!error)
		{
			if (node->config.logging.bulk_pull_logging ())
			{
				node->logger.try_log (boost::str (boost::format ("Received bulk pull for %1% down to %2%, maximum of %3%") % request->start.to_string () % request->end.to_string () % (request->count ? request->count : std::numeric_limits<double>::infinity ())));
			}
			if (is_bootstrap_connection ())
			{
				add_request (std::unique_ptr<nano::message> (request.release ()));
			}
			receive ();
		}
	}
}

void nano::bootstrap_server::receive_bulk_pull_account_action (boost::system::error_code const & ec, size_t size_a, nano::message_header const & header_a)
{
	if (!ec)
	{
		auto error (false);
		assert (size_a == header_a.payload_length_bytes ());
		nano::bufferstream stream (receive_buffer->data (), size_a);
		std::unique_ptr<nano::bulk_pull_account> request (new nano::bulk_pull_account (error, stream, header_a));
		if (!error)
		{
			if (node->config.logging.bulk_pull_logging ())
			{
				node->logger.try_log (boost::str (boost::format ("Received bulk pull account for %1% with a minimum amount of %2%") % request->account.to_account () % nano::amount (request->minimum_amount).format_balance (nano::Mxrb_ratio, 10, true)));
			}
			if (is_bootstrap_connection ())
			{
				add_request (std::unique_ptr<nano::message> (request.release ()));
			}
			receive ();
		}
	}
}

void nano::bootstrap_server::receive_frontier_req_action (boost::system::error_code const & ec, size_t size_a, nano::message_header const & header_a)
{
	if (!ec)
	{
		auto error (false);
		nano::bufferstream stream (receive_buffer->data (), size_a);
		std::unique_ptr<nano::frontier_req> request (new nano::frontier_req (error, stream, header_a));
		if (!error)
		{
			if (node->config.logging.bulk_pull_logging ())
			{
				node->logger.try_log (boost::str (boost::format ("Received frontier request for %1% with age %2%") % request->start.to_string () % request->age));
			}
			if (is_bootstrap_connection ())
			{
				add_request (std::unique_ptr<nano::message> (request.release ()));
			}
			receive ();
		}
	}
	else
	{
		if (node->config.logging.network_logging ())
		{
			node->logger.try_log (boost::str (boost::format ("Error sending receiving frontier request: %1%") % ec.message ()));
		}
	}
}

void nano::bootstrap_server::receive_keepalive_action (boost::system::error_code const & ec, size_t size_a, nano::message_header const & header_a)
{
	if (!ec)
	{
		auto error (false);
		nano::bufferstream stream (receive_buffer->data (), size_a);
		std::unique_ptr<nano::keepalive> request (new nano::keepalive (error, stream, header_a));
		if (!error)
		{
			if (type == nano::bootstrap_server_type::realtime || type == nano::bootstrap_server_type::realtime_response_server)
			{
				add_request (std::unique_ptr<nano::message> (request.release ()));
			}
			receive ();
		}
	}
	else
	{
		if (node->config.logging.network_keepalive_logging ())
		{
			node->logger.try_log (boost::str (boost::format ("Error receiving keepalive: %1%") % ec.message ()));
		}
	}
}

void nano::bootstrap_server::receive_publish_action (boost::system::error_code const & ec, size_t size_a, nano::message_header const & header_a)
{
	if (!ec)
	{
		auto error (false);
		nano::bufferstream stream (receive_buffer->data (), size_a);
		std::unique_ptr<nano::publish> request (new nano::publish (error, stream, header_a));
		if (!error)
		{
			if (type == nano::bootstrap_server_type::realtime || type == nano::bootstrap_server_type::realtime_response_server)
			{
				add_request (std::unique_ptr<nano::message> (request.release ()));
			}
			receive ();
		}
	}
	else
	{
		if (node->config.logging.network_message_logging ())
		{
			node->logger.try_log (boost::str (boost::format ("Error receiving publish: %1%") % ec.message ()));
		}
	}
}

void nano::bootstrap_server::receive_confirm_req_action (boost::system::error_code const & ec, size_t size_a, nano::message_header const & header_a)
{
	if (!ec)
	{
		auto error (false);
		nano::bufferstream stream (receive_buffer->data (), size_a);
		std::unique_ptr<nano::confirm_req> request (new nano::confirm_req (error, stream, header_a));
		if (!error)
		{
			if (type == nano::bootstrap_server_type::realtime || type == nano::bootstrap_server_type::realtime_response_server)
			{
				add_request (std::unique_ptr<nano::message> (request.release ()));
			}
			receive ();
		}
	}
	else if (node->config.logging.network_message_logging ())
	{
		node->logger.try_log (boost::str (boost::format ("Error receiving confirm_req: %1%") % ec.message ()));
	}
}

void nano::bootstrap_server::receive_confirm_ack_action (boost::system::error_code const & ec, size_t size_a, nano::message_header const & header_a)
{
	if (!ec)
	{
		auto error (false);
		nano::bufferstream stream (receive_buffer->data (), size_a);
		std::unique_ptr<nano::confirm_ack> request (new nano::confirm_ack (error, stream, header_a));
		if (!error)
		{
			if (type == nano::bootstrap_server_type::realtime || type == nano::bootstrap_server_type::realtime_response_server)
			{
				add_request (std::unique_ptr<nano::message> (request.release ()));
			}
			receive ();
		}
	}
	else if (node->config.logging.network_message_logging ())
	{
		node->logger.try_log (boost::str (boost::format ("Error receiving confirm_ack: %1%") % ec.message ()));
	}
}

void nano::bootstrap_server::receive_node_id_handshake_action (boost::system::error_code const & ec, size_t size_a, nano::message_header const & header_a)
{
	if (!ec)
	{
		auto error (false);
		nano::bufferstream stream (receive_buffer->data (), size_a);
		std::unique_ptr<nano::node_id_handshake> request (new nano::node_id_handshake (error, stream, header_a));
		if (!error)
		{
			if (type == nano::bootstrap_server_type::undefined && !node->flags.disable_tcp_realtime)
			{
				add_request (std::unique_ptr<nano::message> (request.release ()));
			}
			receive ();
		}
	}
	else if (node->config.logging.network_node_id_handshake_logging ())
	{
		node->logger.try_log (boost::str (boost::format ("Error receiving node_id_handshake: %1%") % ec.message ()));
	}
}

void nano::bootstrap_server::add_request (std::unique_ptr<nano::message> message_a)
{
	assert (message_a != nullptr);
	std::lock_guard<std::mutex> lock (mutex);
	auto start (requests.empty ());
	requests.push (std::move (message_a));
	if (start)
	{
		run_next ();
	}
}

void nano::bootstrap_server::finish_request ()
{
	std::lock_guard<std::mutex> lock (mutex);
	requests.pop ();
	if (!requests.empty ())
	{
		run_next ();
	}
	else
	{
		std::weak_ptr<nano::bootstrap_server> this_w (shared_from_this ());
		node->alarm.add (std::chrono::steady_clock::now () + (node->config.tcp_io_timeout * 2) + std::chrono::seconds (1), [this_w]() {
			if (auto this_l = this_w.lock ())
			{
				this_l->timeout ();
			}
		});
	}
}

void nano::bootstrap_server::finish_request_async ()
{
	std::weak_ptr<nano::bootstrap_server> this_w (shared_from_this ());
	node->background ([this_w]() {
		if (auto this_l = this_w.lock ())
		{
			this_l->finish_request ();
		}
	});
}

void nano::bootstrap_server::timeout ()
{
	if (socket != nullptr)
	{
		if (socket->has_timed_out ())
		{
			if (node->config.logging.bulk_pull_logging ())
			{
				node->logger.try_log ("Closing incoming tcp / bootstrap server by timeout");
			}
			{
				std::lock_guard<std::mutex> lock (node->bootstrap.mutex);
				node->bootstrap.connections.erase (this);
			}
			socket->close ();
		}
	}
	else
	{
		std::lock_guard<std::mutex> lock (node->bootstrap.mutex);
		node->bootstrap.connections.erase (this);
	}
}

namespace
{
class request_response_visitor : public nano::message_visitor
{
public:
	explicit request_response_visitor (std::shared_ptr<nano::bootstrap_server> const & connection_a) :
	connection (connection_a)
	{
	}
	virtual ~request_response_visitor () = default;
	void keepalive (nano::keepalive const & message_a) override
	{
		bool first_keepalive (connection->keepalive_first);
		if (first_keepalive)
		{
			connection->keepalive_first = false;
		}
		connection->finish_request_async ();
		auto connection_l (connection->shared_from_this ());
		connection->node->background ([connection_l, message_a, first_keepalive]() {
			connection_l->node->network.tcp_channels.process_keepalive (message_a, connection_l->remote_endpoint, first_keepalive);
		});
	}
	void publish (nano::publish const & message_a) override
	{
		connection->finish_request_async ();
		auto connection_l (connection->shared_from_this ());
		connection->node->background ([connection_l, message_a]() {
			connection_l->node->network.tcp_channels.process_message (message_a, connection_l->remote_endpoint, connection_l->remote_node_id, connection_l->socket, connection_l->type);
		});
	}
	void confirm_req (nano::confirm_req const & message_a) override
	{
		connection->finish_request_async ();
		auto connection_l (connection->shared_from_this ());
		connection->node->background ([connection_l, message_a]() {
			connection_l->node->network.tcp_channels.process_message (message_a, connection_l->remote_endpoint, connection_l->remote_node_id, connection_l->socket, connection_l->type);
		});
	}
	void confirm_ack (nano::confirm_ack const & message_a) override
	{
		connection->finish_request_async ();
		auto connection_l (connection->shared_from_this ());
		connection->node->background ([connection_l, message_a]() {
			connection_l->node->network.tcp_channels.process_message (message_a, connection_l->remote_endpoint, connection_l->remote_node_id, connection_l->socket, connection_l->type);
		});
	}
	void bulk_pull (nano::bulk_pull const &) override
	{
		auto response (std::make_shared<nano::bulk_pull_server> (connection, std::unique_ptr<nano::bulk_pull> (static_cast<nano::bulk_pull *> (connection->requests.front ().release ()))));
		response->send_next ();
	}
	void bulk_pull_account (nano::bulk_pull_account const &) override
	{
		auto response (std::make_shared<nano::bulk_pull_account_server> (connection, std::unique_ptr<nano::bulk_pull_account> (static_cast<nano::bulk_pull_account *> (connection->requests.front ().release ()))));
		response->send_frontier ();
	}
	void bulk_push (nano::bulk_push const &) override
	{
		auto response (std::make_shared<nano::bulk_push_server> (connection));
		response->throttled_receive ();
	}
	void frontier_req (nano::frontier_req const &) override
	{
		auto response (std::make_shared<nano::frontier_req_server> (connection, std::unique_ptr<nano::frontier_req> (static_cast<nano::frontier_req *> (connection->requests.front ().release ()))));
		response->send_next ();
	}
	void node_id_handshake (nano::node_id_handshake const & message_a) override
	{
		if (connection->node->config.logging.network_node_id_handshake_logging ())
		{
			connection->node->logger.try_log (boost::str (boost::format ("Received node_id_handshake message from %1%") % connection->remote_endpoint));
		}
		if (message_a.query)
		{
			boost::optional<std::pair<nano::account, nano::signature>> response (std::make_pair (connection->node->node_id.pub, nano::sign_message (connection->node->node_id.prv, connection->node->node_id.pub, *message_a.query)));
			assert (!nano::validate_message (response->first, *message_a.query, response->second));
			auto cookie (connection->node->network.syn_cookies.assign (nano::transport::map_tcp_to_endpoint (connection->remote_endpoint)));
			nano::node_id_handshake response_message (cookie, response);
			auto bytes = response_message.to_bytes ();
			// clang-format off
			connection->socket->async_write (bytes, [ bytes, connection = connection ](boost::system::error_code const & ec, size_t size_a) {
				if (ec)
				{
					if (connection->node->config.logging.network_node_id_handshake_logging ())
					{
						connection->node->logger.try_log (boost::str (boost::format ("Error sending node_id_handshake to %1%: %2%") % connection->remote_endpoint % ec.message ()));
					}
					// Stop invalid handshake
					connection->stop ();
				}
				else
				{
					connection->node->stats.inc (nano::stat::type::message, nano::stat::detail::node_id_handshake, nano::stat::dir::out);
					connection->finish_request ();
				}
			});
			// clang-format on
		}
		else if (message_a.response)
		{
			nano::account const & node_id (message_a.response->first);
			if (!connection->node->network.syn_cookies.validate (nano::transport::map_tcp_to_endpoint (connection->remote_endpoint), node_id, message_a.response->second) && node_id != connection->node->node_id.pub)
			{
				connection->remote_node_id = node_id;
				connection->type = nano::bootstrap_server_type::realtime;
				++connection->node->bootstrap.realtime_count;
				connection->finish_request_async ();
			}
			else
			{
				// Stop invalid handshake
				connection->stop ();
			}
		}
		else
		{
			connection->finish_request_async ();
		}
		nano::account node_id (connection->remote_node_id);
		nano::bootstrap_server_type type (connection->type);
		assert (node_id.is_zero () || type == nano::bootstrap_server_type::realtime);
		auto connection_l (connection->shared_from_this ());
		connection->node->background ([connection_l, message_a, node_id, type]() {
			connection_l->node->network.tcp_channels.process_message (message_a, connection_l->remote_endpoint, node_id, connection_l->socket, type);
		});
	}
	std::shared_ptr<nano::bootstrap_server> connection;
};
}

void nano::bootstrap_server::run_next ()
{
	assert (!requests.empty ());
	request_response_visitor visitor (shared_from_this ());
	requests.front ()->visit (visitor);
}

bool nano::bootstrap_server::is_bootstrap_connection ()
{
	if (type == nano::bootstrap_server_type::undefined && !node->flags.disable_bootstrap_listener && node->bootstrap.bootstrap_count < node->config.bootstrap_connections_max)
	{
		++node->bootstrap.bootstrap_count;
		type = nano::bootstrap_server_type::bootstrap;
	}
	return type == nano::bootstrap_server_type::bootstrap;
}

/**
 * Handle a request for the pull of all blocks associated with an account
 * The account is supplied as the "start" member, and the final block to
 * send is the "end" member.  The "start" member may also be a block
 * hash, in which case the that hash is used as the start of a chain
 * to send.  To determine if "start" is interpretted as an account or
 * hash, the ledger is checked to see if the block specified exists,
 * if not then it is interpretted as an account.
 *
 * Additionally, if "start" is specified as a block hash the range
 * is inclusive of that block hash, that is the range will be:
 * [start, end); In the case that a block hash is not specified the
 * range will be exclusive of the frontier for that account with
 * a range of (frontier, end)
 */
void nano::bulk_pull_server::set_current_end ()
{
	include_start = false;
	assert (request != nullptr);
	auto transaction (connection->node->store.tx_begin_read ());
	if (!connection->node->store.block_exists (transaction, request->end))
	{
		if (connection->node->config.logging.bulk_pull_logging ())
		{
			connection->node->logger.try_log (boost::str (boost::format ("Bulk pull end block doesn't exist: %1%, sending everything") % request->end.to_string ()));
		}
		request->end.clear ();
	}

	if (connection->node->store.block_exists (transaction, request->start))
	{
		if (connection->node->config.logging.bulk_pull_logging ())
		{
			connection->node->logger.try_log (boost::str (boost::format ("Bulk pull request for block hash: %1%") % request->start.to_string ()));
		}

		current = request->start;
		include_start = true;
	}
	else
	{
		nano::account_info info;
		auto no_address (connection->node->store.account_get (transaction, request->start, info));
		if (no_address)
		{
			if (connection->node->config.logging.bulk_pull_logging ())
			{
				connection->node->logger.try_log (boost::str (boost::format ("Request for unknown account: %1%") % request->start.to_account ()));
			}
			current = request->end;
		}
		else
		{
			current = info.head;
			if (!request->end.is_zero ())
			{
				auto account (connection->node->ledger.account (transaction, request->end));
				if (account != request->start)
				{
					if (connection->node->config.logging.bulk_pull_logging ())
					{
						connection->node->logger.try_log (boost::str (boost::format ("Request for block that is not on account chain: %1% not on %2%") % request->end.to_string () % request->start.to_account ()));
					}
					current = request->end;
				}
			}
		}
	}

	sent_count = 0;
	if (request->is_count_present ())
	{
		max_count = request->count;
	}
	else
	{
		max_count = 0;
	}
}

void nano::bulk_pull_server::send_next ()
{
	auto block (get_next ());
	if (block != nullptr)
	{
		{
			send_buffer->clear ();
			nano::vectorstream stream (*send_buffer);
			nano::serialize_block (stream, *block);
		}
		auto this_l (shared_from_this ());
		if (connection->node->config.logging.bulk_pull_logging ())
		{
			connection->node->logger.try_log (boost::str (boost::format ("Sending block: %1%") % block->hash ().to_string ()));
		}
		connection->socket->async_write (send_buffer, [this_l](boost::system::error_code const & ec, size_t size_a) {
			this_l->sent_action (ec, size_a);
		});
	}
	else
	{
		send_finished ();
	}
}

std::shared_ptr<nano::block> nano::bulk_pull_server::get_next ()
{
	std::shared_ptr<nano::block> result;
	bool send_current = false, set_current_to_end = false;

	/*
	 * Determine if we should reply with a block
	 *
	 * If our cursor is on the final block, we should signal that we
	 * are done by returning a null result.
	 *
	 * Unless we are including the "start" member and this is the
	 * start member, then include it anyway.
	 */
	if (current != request->end)
	{
		send_current = true;
	}
	else if (current == request->end && include_start == true)
	{
		send_current = true;

		/*
		 * We also need to ensure that the next time
		 * are invoked that we return a null result
		 */
		set_current_to_end = true;
	}

	/*
	 * Account for how many blocks we have provided.  If this
	 * exceeds the requested maximum, return an empty object
	 * to signal the end of results
	 */
	if (max_count != 0 && sent_count >= max_count)
	{
		send_current = false;
	}

	if (send_current)
	{
		auto transaction (connection->node->store.tx_begin_read ());
		result = connection->node->store.block_get (transaction, current);
		if (result != nullptr && set_current_to_end == false)
		{
			auto previous (result->previous ());
			if (!previous.is_zero ())
			{
				current = previous;
			}
			else
			{
				current = request->end;
			}
		}
		else
		{
			current = request->end;
		}

		sent_count++;
	}

	/*
	 * Once we have processed "get_next()" once our cursor is no longer on
	 * the "start" member, so this flag is not relevant is always false.
	 */
	include_start = false;

	return result;
}

void nano::bulk_pull_server::sent_action (boost::system::error_code const & ec, size_t size_a)
{
	if (!ec)
	{
		send_next ();
	}
	else
	{
		if (connection->node->config.logging.bulk_pull_logging ())
		{
			connection->node->logger.try_log (boost::str (boost::format ("Unable to bulk send block: %1%") % ec.message ()));
		}
	}
}

void nano::bulk_pull_server::send_finished ()
{
	send_buffer->clear ();
	send_buffer->push_back (static_cast<uint8_t> (nano::block_type::not_a_block));
	auto this_l (shared_from_this ());
	if (connection->node->config.logging.bulk_pull_logging ())
	{
		connection->node->logger.try_log ("Bulk sending finished");
	}
	connection->socket->async_write (send_buffer, [this_l](boost::system::error_code const & ec, size_t size_a) {
		this_l->no_block_sent (ec, size_a);
	});
}

void nano::bulk_pull_server::no_block_sent (boost::system::error_code const & ec, size_t size_a)
{
	if (!ec)
	{
		assert (size_a == 1);
		connection->finish_request ();
	}
	else
	{
		if (connection->node->config.logging.bulk_pull_logging ())
		{
			connection->node->logger.try_log ("Unable to send not-a-block");
		}
	}
}

nano::bulk_pull_server::bulk_pull_server (std::shared_ptr<nano::bootstrap_server> const & connection_a, std::unique_ptr<nano::bulk_pull> request_a) :
connection (connection_a),
request (std::move (request_a)),
send_buffer (std::make_shared<std::vector<uint8_t>> ())
{
	set_current_end ();
}

/**
 * Bulk pull blocks related to an account
 */
void nano::bulk_pull_account_server::set_params ()
{
	assert (request != nullptr);

	/*
	 * Parse the flags
	 */
	invalid_request = false;
	pending_include_address = false;
	pending_address_only = false;
	if (request->flags == nano::bulk_pull_account_flags::pending_address_only)
	{
		pending_address_only = true;
	}
	else if (request->flags == nano::bulk_pull_account_flags::pending_hash_amount_and_address)
	{
		/**
		 ** This is the same as "pending_hash_and_amount" but with the
		 ** sending address appended, for UI purposes mainly.
		 **/
		pending_include_address = true;
	}
	else if (request->flags == nano::bulk_pull_account_flags::pending_hash_and_amount)
	{
		/** The defaults are set above **/
	}
	else
	{
		if (connection->node->config.logging.bulk_pull_logging ())
		{
			connection->node->logger.try_log (boost::str (boost::format ("Invalid bulk_pull_account flags supplied %1%") % static_cast<uint8_t> (request->flags)));
		}

		invalid_request = true;

		return;
	}

	/*
	 * Initialize the current item from the requested account
	 */
	current_key.account = request->account;
	current_key.hash = 0;
}

void nano::bulk_pull_account_server::send_frontier ()
{
	/*
	 * This function is really the entry point into this class,
	 * so handle the invalid_request case by terminating the
	 * request without any response
	 */
	if (!invalid_request)
	{
		auto stream_transaction (connection->node->store.tx_begin_read ());

		// Get account balance and frontier block hash
		auto account_frontier_hash (connection->node->ledger.latest (stream_transaction, request->account));
		auto account_frontier_balance_int (connection->node->ledger.account_balance (stream_transaction, request->account));
		nano::uint128_union account_frontier_balance (account_frontier_balance_int);

		// Write the frontier block hash and balance into a buffer
		send_buffer->clear ();
		{
			nano::vectorstream output_stream (*send_buffer);

			write (output_stream, account_frontier_hash.bytes);
			write (output_stream, account_frontier_balance.bytes);
		}

		// Send the buffer to the requestor
		auto this_l (shared_from_this ());
		connection->socket->async_write (send_buffer, [this_l](boost::system::error_code const & ec, size_t size_a) {
			this_l->sent_action (ec, size_a);
		});
	}
}

void nano::bulk_pull_account_server::send_next_block ()
{
	/*
	 * Get the next item from the queue, it is a tuple with the key (which
	 * contains the account and hash) and data (which contains the amount)
	 */
	auto block_data (get_next ());
	auto block_info_key (block_data.first.get ());
	auto block_info (block_data.second.get ());

	if (block_info_key != nullptr)
	{
		/*
		 * If we have a new item, emit it to the socket
		 */
		send_buffer->clear ();

		if (pending_address_only)
		{
			nano::vectorstream output_stream (*send_buffer);

			if (connection->node->config.logging.bulk_pull_logging ())
			{
				connection->node->logger.try_log (boost::str (boost::format ("Sending address: %1%") % block_info->source.to_string ()));
			}

			write (output_stream, block_info->source.bytes);
		}
		else
		{
			nano::vectorstream output_stream (*send_buffer);

			if (connection->node->config.logging.bulk_pull_logging ())
			{
				connection->node->logger.try_log (boost::str (boost::format ("Sending block: %1%") % block_info_key->hash.to_string ()));
			}

			write (output_stream, block_info_key->hash.bytes);
			write (output_stream, block_info->amount.bytes);

			if (pending_include_address)
			{
				/**
				 ** Write the source address as well, if requested
				 **/
				write (output_stream, block_info->source.bytes);
			}
		}

		auto this_l (shared_from_this ());
		connection->socket->async_write (send_buffer, [this_l](boost::system::error_code const & ec, size_t size_a) {
			this_l->sent_action (ec, size_a);
		});
	}
	else
	{
		/*
		 * Otherwise, finalize the connection
		 */
		if (connection->node->config.logging.bulk_pull_logging ())
		{
			connection->node->logger.try_log (boost::str (boost::format ("Done sending blocks")));
		}

		send_finished ();
	}
}

std::pair<std::unique_ptr<nano::pending_key>, std::unique_ptr<nano::pending_info>> nano::bulk_pull_account_server::get_next ()
{
	std::pair<std::unique_ptr<nano::pending_key>, std::unique_ptr<nano::pending_info>> result;

	while (true)
	{
		/*
		 * For each iteration of this loop, establish and then
		 * destroy a database transaction, to avoid locking the
		 * database for a prolonged period.
		 */
		auto stream_transaction (connection->node->store.tx_begin_read ());
		auto stream (connection->node->store.pending_begin (stream_transaction, current_key));

		if (stream == nano::store_iterator<nano::pending_key, nano::pending_info> (nullptr))
		{
			break;
		}

		nano::pending_key key (stream->first);
		nano::pending_info info (stream->second);

		/*
		 * Get the key for the next value, to use in the next call or iteration
		 */
		current_key.account = key.account;
		current_key.hash = key.hash.number () + 1;

		/*
		 * Finish up if the response is for a different account
		 */
		if (key.account != request->account)
		{
			break;
		}

		/*
		 * Skip entries where the amount is less than the requested
		 * minimum
		 */
		if (info.amount < request->minimum_amount)
		{
			continue;
		}

		/*
		 * If the pending_address_only flag is set, de-duplicate the
		 * responses.  The responses are the address of the sender,
		 * so they are are part of the pending table's information
		 * and not key, so we have to de-duplicate them manually.
		 */
		if (pending_address_only)
		{
			if (!deduplication.insert (info.source).second)
			{
				/*
				 * If the deduplication map gets too
				 * large, clear it out.  This may
				 * result in some duplicates getting
				 * sent to the client, but we do not
				 * want to commit too much memory
				 */
				if (deduplication.size () > 4096)
				{
					deduplication.clear ();
				}
				continue;
			}
		}

		result.first = std::unique_ptr<nano::pending_key> (new nano::pending_key (key));
		result.second = std::unique_ptr<nano::pending_info> (new nano::pending_info (info));

		break;
	}

	return result;
}

void nano::bulk_pull_account_server::sent_action (boost::system::error_code const & ec, size_t size_a)
{
	if (!ec)
	{
		send_next_block ();
	}
	else
	{
		if (connection->node->config.logging.bulk_pull_logging ())
		{
			connection->node->logger.try_log (boost::str (boost::format ("Unable to bulk send block: %1%") % ec.message ()));
		}
	}
}

void nano::bulk_pull_account_server::send_finished ()
{
	/*
	 * The "bulk_pull_account" final sequence is a final block of all
	 * zeros.  If we are sending only account public keys (with the
	 * "pending_address_only" flag) then it will be 256-bits of zeros,
	 * otherwise it will be either 384-bits of zeros (if the
	 * "pending_include_address" flag is not set) or 640-bits of zeros
	 * (if that flag is set).
	 */
	send_buffer->clear ();

	{
		nano::vectorstream output_stream (*send_buffer);
		nano::uint256_union account_zero (0);
		nano::uint128_union balance_zero (0);

		write (output_stream, account_zero.bytes);

		if (!pending_address_only)
		{
			write (output_stream, balance_zero.bytes);
			if (pending_include_address)
			{
				write (output_stream, account_zero.bytes);
			}
		}
	}

	auto this_l (shared_from_this ());

	if (connection->node->config.logging.bulk_pull_logging ())
	{
		connection->node->logger.try_log ("Bulk sending for an account finished");
	}

	connection->socket->async_write (send_buffer, [this_l](boost::system::error_code const & ec, size_t size_a) {
		this_l->complete (ec, size_a);
	});
}

void nano::bulk_pull_account_server::complete (boost::system::error_code const & ec, size_t size_a)
{
	if (!ec)
	{
		if (pending_address_only)
		{
			assert (size_a == 32);
		}
		else
		{
			if (pending_include_address)
			{
				assert (size_a == 80);
			}
			else
			{
				assert (size_a == 48);
			}
		}

		connection->finish_request ();
	}
	else
	{
		if (connection->node->config.logging.bulk_pull_logging ())
		{
			connection->node->logger.try_log ("Unable to pending-as-zero");
		}
	}
}

nano::bulk_pull_account_server::bulk_pull_account_server (std::shared_ptr<nano::bootstrap_server> const & connection_a, std::unique_ptr<nano::bulk_pull_account> request_a) :
connection (connection_a),
request (std::move (request_a)),
send_buffer (std::make_shared<std::vector<uint8_t>> ()),
current_key (0, 0)
{
	/*
	 * Setup the streaming response for the first call to "send_frontier" and  "send_next_block"
	 */
	set_params ();
}

nano::bulk_push_server::bulk_push_server (std::shared_ptr<nano::bootstrap_server> const & connection_a) :
receive_buffer (std::make_shared<std::vector<uint8_t>> ()),
connection (connection_a)
{
	receive_buffer->resize (256);
}

void nano::bulk_push_server::throttled_receive ()
{
	if (!connection->node->block_processor.half_full ())
	{
		receive ();
	}
	else
	{
		auto this_l (shared_from_this ());
		connection->node->alarm.add (std::chrono::steady_clock::now () + std::chrono::seconds (1), [this_l]() {
			if (!this_l->connection->stopped)
			{
				this_l->throttled_receive ();
			}
		});
	}
}

void nano::bulk_push_server::receive ()
{
	if (connection->node->bootstrap_initiator.in_progress ())
	{
		if (connection->node->config.logging.bulk_pull_logging ())
		{
			connection->node->logger.try_log ("Aborting bulk_push because a bootstrap attempt is in progress");
		}
	}
	else
	{
		auto this_l (shared_from_this ());
		connection->socket->async_read (receive_buffer, 1, [this_l](boost::system::error_code const & ec, size_t size_a) {
			if (!ec)
			{
				this_l->received_type ();
			}
			else
			{
				if (this_l->connection->node->config.logging.bulk_pull_logging ())
				{
					this_l->connection->node->logger.try_log (boost::str (boost::format ("Error receiving block type: %1%") % ec.message ()));
				}
			}
		});
	}
}

void nano::bulk_push_server::received_type ()
{
	auto this_l (shared_from_this ());
	nano::block_type type (static_cast<nano::block_type> (receive_buffer->data ()[0]));
	switch (type)
	{
		case nano::block_type::send:
		{
			connection->node->stats.inc (nano::stat::type::bootstrap, nano::stat::detail::send, nano::stat::dir::in);
			connection->socket->async_read (receive_buffer, nano::send_block::size, [this_l, type](boost::system::error_code const & ec, size_t size_a) {
				this_l->received_block (ec, size_a, type);
			});
			break;
		}
		case nano::block_type::receive:
		{
			connection->node->stats.inc (nano::stat::type::bootstrap, nano::stat::detail::receive, nano::stat::dir::in);
			connection->socket->async_read (receive_buffer, nano::receive_block::size, [this_l, type](boost::system::error_code const & ec, size_t size_a) {
				this_l->received_block (ec, size_a, type);
			});
			break;
		}
		case nano::block_type::open:
		{
			connection->node->stats.inc (nano::stat::type::bootstrap, nano::stat::detail::open, nano::stat::dir::in);
			connection->socket->async_read (receive_buffer, nano::open_block::size, [this_l, type](boost::system::error_code const & ec, size_t size_a) {
				this_l->received_block (ec, size_a, type);
			});
			break;
		}
		case nano::block_type::change:
		{
			connection->node->stats.inc (nano::stat::type::bootstrap, nano::stat::detail::change, nano::stat::dir::in);
			connection->socket->async_read (receive_buffer, nano::change_block::size, [this_l, type](boost::system::error_code const & ec, size_t size_a) {
				this_l->received_block (ec, size_a, type);
			});
			break;
		}
		case nano::block_type::state:
		{
			connection->node->stats.inc (nano::stat::type::bootstrap, nano::stat::detail::state_block, nano::stat::dir::in);
			connection->socket->async_read (receive_buffer, nano::state_block::size, [this_l, type](boost::system::error_code const & ec, size_t size_a) {
				this_l->received_block (ec, size_a, type);
			});
			break;
		}
		case nano::block_type::not_a_block:
		{
			connection->finish_request ();
			break;
		}
		default:
		{
			if (connection->node->config.logging.network_packet_logging ())
			{
				connection->node->logger.try_log ("Unknown type received as block type");
			}
			break;
		}
	}
}

void nano::bulk_push_server::received_block (boost::system::error_code const & ec, size_t size_a, nano::block_type type_a)
{
	if (!ec)
	{
		nano::bufferstream stream (receive_buffer->data (), size_a);
		auto block (nano::deserialize_block (stream, type_a));
		if (block != nullptr && !nano::work_validate (*block))
		{
			connection->node->process_active (std::move (block));
			throttled_receive ();
		}
		else
		{
			if (connection->node->config.logging.bulk_pull_logging ())
			{
				connection->node->logger.try_log ("Error deserializing block received from pull request");
			}
		}
	}
}

nano::frontier_req_server::frontier_req_server (std::shared_ptr<nano::bootstrap_server> const & connection_a, std::unique_ptr<nano::frontier_req> request_a) :
connection (connection_a),
current (request_a->start.number () - 1),
frontier (0),
request (std::move (request_a)),
send_buffer (std::make_shared<std::vector<uint8_t>> ()),
count (0)
{
	next ();
}

void nano::frontier_req_server::send_next ()
{
	if (!current.is_zero () && count < request->count)
	{
		{
			send_buffer->clear ();
			nano::vectorstream stream (*send_buffer);
			write (stream, current.bytes);
			write (stream, frontier.bytes);
		}
		auto this_l (shared_from_this ());
		if (connection->node->config.logging.bulk_pull_logging ())
		{
			connection->node->logger.try_log (boost::str (boost::format ("Sending frontier for %1% %2%") % current.to_account () % frontier.to_string ()));
		}
		next ();
		connection->socket->async_write (send_buffer, [this_l](boost::system::error_code const & ec, size_t size_a) {
			this_l->sent_action (ec, size_a);
		});
	}
	else
	{
		send_finished ();
	}
}

void nano::frontier_req_server::send_finished ()
{
	{
		send_buffer->clear ();
		nano::vectorstream stream (*send_buffer);
		nano::uint256_union zero (0);
		write (stream, zero.bytes);
		write (stream, zero.bytes);
	}
	auto this_l (shared_from_this ());
	if (connection->node->config.logging.network_logging ())
	{
		connection->node->logger.try_log ("Frontier sending finished");
	}
	connection->socket->async_write (send_buffer, [this_l](boost::system::error_code const & ec, size_t size_a) {
		this_l->no_block_sent (ec, size_a);
	});
}

void nano::frontier_req_server::no_block_sent (boost::system::error_code const & ec, size_t size_a)
{
	if (!ec)
	{
		connection->finish_request ();
	}
	else
	{
		if (connection->node->config.logging.network_logging ())
		{
			connection->node->logger.try_log (boost::str (boost::format ("Error sending frontier finish: %1%") % ec.message ()));
		}
	}
}

void nano::frontier_req_server::sent_action (boost::system::error_code const & ec, size_t size_a)
{
	if (!ec)
	{
		count++;
		send_next ();
	}
	else
	{
		if (connection->node->config.logging.network_logging ())
		{
			connection->node->logger.try_log (boost::str (boost::format ("Error sending frontier pair: %1%") % ec.message ()));
		}
	}
}

void nano::frontier_req_server::next ()
{
	// Filling accounts deque to prevent often read transactions
	if (accounts.empty ())
	{
		auto now (nano::seconds_since_epoch ());
		bool skip_old (request->age != std::numeric_limits<decltype (request->age)>::max ());
		size_t max_size (128);
		auto transaction (connection->node->store.tx_begin_read ());
		for (auto i (connection->node->store.latest_begin (transaction, current.number () + 1)), n (connection->node->store.latest_end ()); i != n && accounts.size () != max_size; ++i)
		{
			nano::account_info const & info (i->second);
			if (!skip_old || (now - info.modified) <= request->age)
			{
				nano::account const & account (i->first);
				accounts.emplace_back (account, info.head);
			}
		}
		/* If loop breaks before max_size, then latest_end () is reached
		Add empty record to finish frontier_req_server */
		if (accounts.size () != max_size)
		{
			accounts.emplace_back (nano::account (0), nano::block_hash (0));
		}
	}
	// Retrieving accounts from deque
	auto const & account_pair (accounts.front ());
	current = account_pair.first;
	frontier = account_pair.second;
	accounts.pop_front ();
}

void nano::pulls_cache::add (nano::pull_info const & pull_a)
{
	if (pull_a.processed > 500)
	{
		std::lock_guard<std::mutex> guard (pulls_cache_mutex);
		// Clean old pull
		if (cache.size () > cache_size_max)
		{
			cache.erase (cache.begin ());
		}
		assert (cache.size () <= cache_size_max);
		nano::uint512_union head_512 (pull_a.account, pull_a.head_original);
		auto existing (cache.get<account_head_tag> ().find (head_512));
		if (existing == cache.get<account_head_tag> ().end ())
		{
			// Insert new pull
			auto inserted (cache.insert (nano::cached_pulls{ std::chrono::steady_clock::now (), head_512, pull_a.head }));
			(void)inserted;
			assert (inserted.second);
		}
		else
		{
			// Update existing pull
			cache.get<account_head_tag> ().modify (existing, [pull_a](nano::cached_pulls & cache_a) {
				cache_a.time = std::chrono::steady_clock::now ();
				cache_a.new_head = pull_a.head;
			});
		}
	}
}

void nano::pulls_cache::update_pull (nano::pull_info & pull_a)
{
	std::lock_guard<std::mutex> guard (pulls_cache_mutex);
	nano::uint512_union head_512 (pull_a.account, pull_a.head_original);
	auto existing (cache.get<account_head_tag> ().find (head_512));
	if (existing != cache.get<account_head_tag> ().end ())
	{
		pull_a.head = existing->new_head;
	}
}

void nano::pulls_cache::remove (nano::pull_info const & pull_a)
{
	std::lock_guard<std::mutex> guard (pulls_cache_mutex);
	nano::uint512_union head_512 (pull_a.account, pull_a.head_original);
	cache.get<account_head_tag> ().erase (head_512);
}

namespace nano
{
std::unique_ptr<seq_con_info_component> collect_seq_con_info (pulls_cache & pulls_cache, const std::string & name)
{
	size_t cache_count = 0;

	{
		std::lock_guard<std::mutex> guard (pulls_cache.pulls_cache_mutex);
		cache_count = pulls_cache.cache.size ();
	}
	auto sizeof_element = sizeof (decltype (pulls_cache.cache)::value_type);
	auto composite = std::make_unique<seq_con_info_composite> (name);
	composite->add_component (std::make_unique<seq_con_info_leaf> (seq_con_info{ "pulls_cache", cache_count, sizeof_element }));
	return composite;
}
}
