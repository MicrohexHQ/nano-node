#include <nano/core_test/testutil.hpp>
#include <nano/node/testing.hpp>
#include <nano/node/transport/udp.hpp>

#include <gtest/gtest.h>

#include <boost/iostreams/stream_buffer.hpp>
#include <boost/thread.hpp>

using namespace std::chrono_literals;

TEST (network, tcp_connection)
{
	boost::asio::io_context io_ctx;
	boost::asio::ip::tcp::acceptor acceptor (io_ctx);
	boost::asio::ip::tcp::endpoint endpoint (boost::asio::ip::address_v4::any (), 24000);
	acceptor.open (endpoint.protocol ());
	acceptor.set_option (boost::asio::ip::tcp::acceptor::reuse_address (true));
	acceptor.bind (endpoint);
	acceptor.listen ();
	boost::asio::ip::tcp::socket incoming (io_ctx);
	std::atomic<bool> done1 (false);
	std::string message1;
	acceptor.async_accept (incoming,
	[&done1, &message1](boost::system::error_code const & ec_a) {
		   if (ec_a)
		   {
			   message1 = ec_a.message ();
			   std::cerr << message1;
		   }
		   done1 = true; });
	boost::asio::ip::tcp::socket connector (io_ctx);
	std::atomic<bool> done2 (false);
	std::string message2;
	connector.async_connect (boost::asio::ip::tcp::endpoint (boost::asio::ip::address_v4::loopback (), 24000),
	[&done2, &message2](boost::system::error_code const & ec_a) {
		if (ec_a)
		{
			message2 = ec_a.message ();
			std::cerr << message2;
		}
		done2 = true;
	});
	while (!done1 || !done2)
	{
		io_ctx.poll ();
	}
	ASSERT_EQ (0, message1.size ());
	ASSERT_EQ (0, message2.size ());
}

TEST (network, construction)
{
	nano::system system (24000, 1);
	ASSERT_EQ (1, system.nodes.size ());
	ASSERT_EQ (24000, system.nodes[0]->network.endpoint ().port ());
}

TEST (network, self_discard)
{
	nano::system system (24000, 1);
	nano::message_buffer data;
	data.endpoint = system.nodes[0]->network.endpoint ();
	ASSERT_EQ (0, system.nodes[0]->stats.count (nano::stat::type::error, nano::stat::detail::bad_sender));
	system.nodes[0]->network.udp_channels.receive_action (&data);
	ASSERT_EQ (1, system.nodes[0]->stats.count (nano::stat::type::error, nano::stat::detail::bad_sender));
}

TEST (network, send_node_id_handshake)
{
	nano::system system (24000, 1);
	ASSERT_EQ (0, system.nodes[0]->network.size ());
	auto node1 (std::make_shared<nano::node> (system.io_ctx, 24001, nano::unique_path (), system.alarm, system.logging, system.work));
	node1->start ();
	system.nodes.push_back (node1);
	auto initial (system.nodes[0]->stats.count (nano::stat::type::message, nano::stat::detail::node_id_handshake, nano::stat::dir::in));
	auto initial_node1 (node1->stats.count (nano::stat::type::message, nano::stat::detail::node_id_handshake, nano::stat::dir::in));
	auto channel (std::make_shared<nano::transport::channel_udp> (system.nodes[0]->network.udp_channels, node1->network.endpoint (), node1->network_params.protocol.protocol_version));
	system.nodes[0]->network.send_keepalive (channel);
	ASSERT_EQ (0, system.nodes[0]->network.size ());
	ASSERT_EQ (0, node1->network.size ());
	system.deadline_set (10s);
	while (node1->stats.count (nano::stat::type::message, nano::stat::detail::node_id_handshake, nano::stat::dir::in) == initial_node1)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	ASSERT_EQ (0, system.nodes[0]->network.size ());
	ASSERT_EQ (1, node1->network.size ());
	system.deadline_set (10s);
	while (system.nodes[0]->stats.count (nano::stat::type::message, nano::stat::detail::node_id_handshake, nano::stat::dir::in) < initial + 2)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	ASSERT_EQ (1, system.nodes[0]->network.size ());
	ASSERT_EQ (1, node1->network.size ());
	auto list1 (system.nodes[0]->network.list (1));
	ASSERT_EQ (node1->network.endpoint (), list1[0]->get_endpoint ());
	auto list2 (node1->network.list (1));
	ASSERT_EQ (system.nodes[0]->network.endpoint (), list2[0]->get_endpoint ());
	node1->stop ();
}

TEST (network, send_node_id_handshake_tcp)
{
	nano::system system (24000, 1);
	ASSERT_EQ (0, system.nodes[0]->network.size ());
	auto node1 (std::make_shared<nano::node> (system.io_ctx, 24001, nano::unique_path (), system.alarm, system.logging, system.work));
	node1->start ();
	system.nodes.push_back (node1);
	auto initial (system.nodes[0]->stats.count (nano::stat::type::message, nano::stat::detail::node_id_handshake, nano::stat::dir::in));
	auto initial_node1 (node1->stats.count (nano::stat::type::message, nano::stat::detail::node_id_handshake, nano::stat::dir::in));
	auto initial_keepalive (system.nodes[0]->stats.count (nano::stat::type::message, nano::stat::detail::keepalive, nano::stat::dir::in));
	std::weak_ptr<nano::node> node_w (system.nodes[0]);
	system.nodes[0]->network.tcp_channels.start_tcp (node1->network.endpoint (), [node_w](std::shared_ptr<nano::transport::channel> channel_a) {
		if (auto node_l = node_w.lock ())
		{
			node_l->network.send_keepalive (channel_a);
		}
	});
	ASSERT_EQ (0, system.nodes[0]->network.size ());
	ASSERT_EQ (0, node1->network.size ());
	system.deadline_set (10s);
	while (system.nodes[0]->stats.count (nano::stat::type::message, nano::stat::detail::node_id_handshake, nano::stat::dir::in) < initial + 2)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	system.deadline_set (5s);
	while (node1->stats.count (nano::stat::type::message, nano::stat::detail::node_id_handshake, nano::stat::dir::in) < initial_node1 + 2)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	system.deadline_set (5s);
	while (system.nodes[0]->network.response_channels.size () != 1 || node1->network.response_channels.size () != 1)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	system.deadline_set (5s);
	while (system.nodes[0]->stats.count (nano::stat::type::message, nano::stat::detail::keepalive, nano::stat::dir::in) < initial_keepalive + 2)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	system.deadline_set (5s);
	while (node1->stats.count (nano::stat::type::message, nano::stat::detail::keepalive, nano::stat::dir::in) < initial_keepalive + 2)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	ASSERT_EQ (1, system.nodes[0]->network.size ());
	ASSERT_EQ (1, node1->network.size ());
	auto list1 (system.nodes[0]->network.list (1));
	ASSERT_EQ (nano::transport::transport_type::tcp, list1[0]->get_type ());
	ASSERT_EQ (node1->network.endpoint (), list1[0]->get_endpoint ());
	auto list2 (node1->network.list (1));
	ASSERT_EQ (nano::transport::transport_type::tcp, list2[0]->get_type ());
	ASSERT_EQ (system.nodes[0]->network.endpoint (), list2[0]->get_endpoint ());
	node1->stop ();
}

TEST (network, last_contacted)
{
	nano::system system (24000, 1);
	ASSERT_EQ (0, system.nodes[0]->network.size ());
	auto node1 (std::make_shared<nano::node> (system.io_ctx, 24001, nano::unique_path (), system.alarm, system.logging, system.work));
	node1->start ();
	system.nodes.push_back (node1);
	auto channel1 (std::make_shared<nano::transport::channel_udp> (node1->network.udp_channels, nano::endpoint (boost::asio::ip::address_v6::loopback (), 24000), node1->network_params.protocol.protocol_version));
	node1->network.send_keepalive (channel1);
	system.deadline_set (10s);

	// Wait until the handshake is complete
	while (system.nodes[0]->network.size () < 1)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	ASSERT_EQ (system.nodes[0]->network.size (), 1);

	auto channel2 (system.nodes[0]->network.udp_channels.channel (nano::endpoint (boost::asio::ip::address_v6::loopback (), 24001)));
	ASSERT_NE (nullptr, channel2);
	// Make sure last_contact gets updated on receiving a non-handshake message
	auto timestamp_before_keepalive = channel2->get_last_packet_received ();
	node1->network.send_keepalive (channel1);
	while (system.nodes[0]->stats.count (nano::stat::type::message, nano::stat::detail::keepalive, nano::stat::dir::in) < 2)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	ASSERT_EQ (system.nodes[0]->network.size (), 1);
	auto timestamp_after_keepalive = channel2->get_last_packet_received ();
	ASSERT_GT (timestamp_after_keepalive, timestamp_before_keepalive);

	node1->stop ();
}

TEST (network, multi_keepalive)
{
	nano::system system (24000, 1);
	ASSERT_EQ (0, system.nodes[0]->network.size ());
	auto node1 (std::make_shared<nano::node> (system.io_ctx, 24001, nano::unique_path (), system.alarm, system.logging, system.work));
	ASSERT_FALSE (node1->init_error ());
	node1->start ();
	system.nodes.push_back (node1);
	ASSERT_EQ (0, node1->network.size ());
	auto channel1 (std::make_shared<nano::transport::channel_udp> (node1->network.udp_channels, system.nodes[0]->network.endpoint (), node1->network_params.protocol.protocol_version));
	node1->network.send_keepalive (channel1);
	ASSERT_EQ (0, node1->network.size ());
	ASSERT_EQ (0, system.nodes[0]->network.size ());
	system.deadline_set (10s);
	while (system.nodes[0]->network.size () != 1)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	auto node2 (std::make_shared<nano::node> (system.io_ctx, 24002, nano::unique_path (), system.alarm, system.logging, system.work));
	ASSERT_FALSE (node2->init_error ());
	node2->start ();
	system.nodes.push_back (node2);
	auto channel2 (std::make_shared<nano::transport::channel_udp> (node2->network.udp_channels, system.nodes[0]->network.endpoint (), node2->network_params.protocol.protocol_version));
	node2->network.send_keepalive (channel2);
	system.deadline_set (10s);
	while (node1->network.size () != 2 || system.nodes[0]->network.size () != 2 || node2->network.size () != 2)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	node1->stop ();
	node2->stop ();
}

TEST (network, send_discarded_publish)
{
	nano::system system (24000, 2);
	auto block (std::make_shared<nano::send_block> (1, 1, 2, nano::keypair ().prv, 4, system.work.generate (1)));
	nano::genesis genesis;
	{
		auto transaction (system.nodes[0]->store.tx_begin_read ());
		system.nodes[0]->network.flood_block (block);
		ASSERT_EQ (genesis.hash (), system.nodes[0]->ledger.latest (transaction, nano::test_genesis_key.pub));
		ASSERT_EQ (genesis.hash (), system.nodes[1]->latest (nano::test_genesis_key.pub));
	}
	system.deadline_set (10s);
	while (system.nodes[1]->stats.count (nano::stat::type::message, nano::stat::detail::publish, nano::stat::dir::in) == 0)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	auto transaction (system.nodes[0]->store.tx_begin_read ());
	ASSERT_EQ (genesis.hash (), system.nodes[0]->ledger.latest (transaction, nano::test_genesis_key.pub));
	ASSERT_EQ (genesis.hash (), system.nodes[1]->latest (nano::test_genesis_key.pub));
}

TEST (network, send_invalid_publish)
{
	nano::system system (24000, 2);
	nano::genesis genesis;
	auto block (std::make_shared<nano::send_block> (1, 1, 20, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (1)));
	{
		auto transaction (system.nodes[0]->store.tx_begin_read ());
		system.nodes[0]->network.flood_block (block);
		ASSERT_EQ (genesis.hash (), system.nodes[0]->ledger.latest (transaction, nano::test_genesis_key.pub));
		ASSERT_EQ (genesis.hash (), system.nodes[1]->latest (nano::test_genesis_key.pub));
	}
	system.deadline_set (10s);
	while (system.nodes[1]->stats.count (nano::stat::type::message, nano::stat::detail::publish, nano::stat::dir::in) == 0)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	auto transaction (system.nodes[0]->store.tx_begin_read ());
	ASSERT_EQ (genesis.hash (), system.nodes[0]->ledger.latest (transaction, nano::test_genesis_key.pub));
	ASSERT_EQ (genesis.hash (), system.nodes[1]->latest (nano::test_genesis_key.pub));
}

TEST (network, send_valid_confirm_ack)
{
	std::vector<nano::transport::transport_type> types{ nano::transport::transport_type::tcp, nano::transport::transport_type::udp };
	for (auto & type : types)
	{
		nano::system system (24000, 2, type);
		nano::keypair key2;
		system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
		system.wallet (1)->insert_adhoc (key2.prv);
		nano::block_hash latest1 (system.nodes[0]->latest (nano::test_genesis_key.pub));
		nano::send_block block2 (latest1, key2.pub, 50, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (latest1));
		nano::block_hash latest2 (system.nodes[1]->latest (nano::test_genesis_key.pub));
		system.nodes[0]->process_active (std::make_shared<nano::send_block> (block2));
		system.deadline_set (10s);
		// Keep polling until latest block changes
		while (system.nodes[1]->latest (nano::test_genesis_key.pub) == latest2)
		{
			ASSERT_NO_ERROR (system.poll ());
		}
		// Make sure the balance has decreased after processing the block.
		ASSERT_EQ (50, system.nodes[1]->balance (nano::test_genesis_key.pub));
	}
}

TEST (network, send_valid_publish)
{
	std::vector<nano::transport::transport_type> types{ nano::transport::transport_type::tcp, nano::transport::transport_type::udp };
	for (auto & type : types)
	{
		nano::system system (24000, 2, type);
		system.nodes[0]->bootstrap_initiator.stop ();
		system.nodes[1]->bootstrap_initiator.stop ();
		system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
		nano::keypair key2;
		system.wallet (1)->insert_adhoc (key2.prv);
		nano::block_hash latest1 (system.nodes[0]->latest (nano::test_genesis_key.pub));
		nano::send_block block2 (latest1, key2.pub, 50, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (latest1));
		auto hash2 (block2.hash ());
		nano::block_hash latest2 (system.nodes[1]->latest (nano::test_genesis_key.pub));
		system.nodes[1]->process_active (std::make_shared<nano::send_block> (block2));
		system.deadline_set (10s);
		while (system.nodes[0]->stats.count (nano::stat::type::message, nano::stat::detail::publish, nano::stat::dir::in) == 0)
		{
			ASSERT_NO_ERROR (system.poll ());
		}
		ASSERT_NE (hash2, latest2);
		system.deadline_set (10s);
		while (system.nodes[1]->latest (nano::test_genesis_key.pub) == latest2)
		{
			ASSERT_NO_ERROR (system.poll ());
		}
		ASSERT_EQ (50, system.nodes[1]->balance (nano::test_genesis_key.pub));
	}
}

TEST (network, send_insufficient_work)
{
	nano::system system (24000, 2);
	auto block (std::make_shared<nano::send_block> (0, 1, 20, nano::test_genesis_key.prv, nano::test_genesis_key.pub, 0));
	nano::publish publish (block);
	auto node1 (system.nodes[1]->shared ());
	nano::transport::channel_udp channel (system.nodes[0]->network.udp_channels, system.nodes[1]->network.endpoint (), system.nodes[0]->network_params.protocol.protocol_version);
	channel.send (publish, [](boost::system::error_code const & ec, size_t size) {});
	ASSERT_EQ (0, system.nodes[0]->stats.count (nano::stat::type::error, nano::stat::detail::insufficient_work));
	system.deadline_set (10s);
	while (system.nodes[1]->stats.count (nano::stat::type::error, nano::stat::detail::insufficient_work) == 0)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	ASSERT_EQ (1, system.nodes[1]->stats.count (nano::stat::type::error, nano::stat::detail::insufficient_work));
}

TEST (receivable_processor, confirm_insufficient_pos)
{
	nano::system system (24000, 1);
	auto & node1 (*system.nodes[0]);
	nano::genesis genesis;
	auto block1 (std::make_shared<nano::send_block> (genesis.hash (), 0, 0, nano::test_genesis_key.prv, nano::test_genesis_key.pub, 0));
	node1.work_generate_blocking (*block1);
	ASSERT_EQ (nano::process_result::progress, node1.process (*block1).code);
	auto node_l (system.nodes[0]);
	node1.active.start (block1);
	nano::keypair key1;
	auto vote (std::make_shared<nano::vote> (key1.pub, key1.prv, 0, block1));
	nano::confirm_ack con1 (vote);
	node1.network.process_message (con1, node1.network.udp_channels.create (node1.network.endpoint ()));
}

TEST (receivable_processor, confirm_sufficient_pos)
{
	nano::system system (24000, 1);
	auto & node1 (*system.nodes[0]);
	nano::genesis genesis;
	auto block1 (std::make_shared<nano::send_block> (genesis.hash (), 0, 0, nano::test_genesis_key.prv, nano::test_genesis_key.pub, 0));
	node1.work_generate_blocking (*block1);
	ASSERT_EQ (nano::process_result::progress, node1.process (*block1).code);
	auto node_l (system.nodes[0]);
	node1.active.start (block1);
	auto vote (std::make_shared<nano::vote> (nano::test_genesis_key.pub, nano::test_genesis_key.prv, 0, block1));
	nano::confirm_ack con1 (vote);
	node1.network.process_message (con1, node1.network.udp_channels.create (node1.network.endpoint ()));
}

TEST (receivable_processor, send_with_receive)
{
	std::vector<nano::transport::transport_type> types{ nano::transport::transport_type::tcp, nano::transport::transport_type::udp };
	for (auto & type : types)
	{
		nano::system system (24000, 2, type);
		auto amount (std::numeric_limits<nano::uint128_t>::max ());
		nano::keypair key2;
		system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
		nano::block_hash latest1 (system.nodes[0]->latest (nano::test_genesis_key.pub));
		system.wallet (1)->insert_adhoc (key2.prv);
		auto block1 (std::make_shared<nano::send_block> (latest1, key2.pub, amount - system.nodes[0]->config.receive_minimum.number (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (latest1)));
		ASSERT_EQ (amount, system.nodes[0]->balance (nano::test_genesis_key.pub));
		ASSERT_EQ (0, system.nodes[0]->balance (key2.pub));
		ASSERT_EQ (amount, system.nodes[1]->balance (nano::test_genesis_key.pub));
		ASSERT_EQ (0, system.nodes[1]->balance (key2.pub));
		system.nodes[0]->process_active (block1);
		system.nodes[0]->block_processor.flush ();
		system.nodes[1]->process_active (block1);
		system.nodes[1]->block_processor.flush ();
		ASSERT_EQ (amount - system.nodes[0]->config.receive_minimum.number (), system.nodes[0]->balance (nano::test_genesis_key.pub));
		ASSERT_EQ (0, system.nodes[0]->balance (key2.pub));
		ASSERT_EQ (amount - system.nodes[0]->config.receive_minimum.number (), system.nodes[1]->balance (nano::test_genesis_key.pub));
		ASSERT_EQ (0, system.nodes[1]->balance (key2.pub));
		system.deadline_set (10s);
		while (system.nodes[0]->balance (key2.pub) != system.nodes[0]->config.receive_minimum.number () || system.nodes[1]->balance (key2.pub) != system.nodes[0]->config.receive_minimum.number ())
		{
			ASSERT_NO_ERROR (system.poll ());
		}
		ASSERT_EQ (amount - system.nodes[0]->config.receive_minimum.number (), system.nodes[0]->balance (nano::test_genesis_key.pub));
		ASSERT_EQ (system.nodes[0]->config.receive_minimum.number (), system.nodes[0]->balance (key2.pub));
		ASSERT_EQ (amount - system.nodes[0]->config.receive_minimum.number (), system.nodes[1]->balance (nano::test_genesis_key.pub));
		ASSERT_EQ (system.nodes[0]->config.receive_minimum.number (), system.nodes[1]->balance (key2.pub));
	}
}

TEST (network, receive_weight_change)
{
	nano::system system (24000, 2);
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	nano::keypair key2;
	system.wallet (1)->insert_adhoc (key2.prv);
	{
		auto transaction (system.nodes[1]->wallets.tx_begin_write ());
		system.wallet (1)->store.representative_set (transaction, key2.pub);
	}
	ASSERT_NE (nullptr, system.wallet (0)->send_action (nano::test_genesis_key.pub, key2.pub, system.nodes[0]->config.receive_minimum.number ()));
	system.deadline_set (10s);
	while (std::any_of (system.nodes.begin (), system.nodes.end (), [&](std::shared_ptr<nano::node> const & node_a) { return node_a->weight (key2.pub) != system.nodes[0]->config.receive_minimum.number (); }))
	{
		ASSERT_NO_ERROR (system.poll ());
	}
}

TEST (parse_endpoint, valid)
{
	std::string string ("::1:24000");
	nano::endpoint endpoint;
	ASSERT_FALSE (nano::parse_endpoint (string, endpoint));
	ASSERT_EQ (boost::asio::ip::address_v6::loopback (), endpoint.address ());
	ASSERT_EQ (24000, endpoint.port ());
}

TEST (parse_endpoint, invalid_port)
{
	std::string string ("::1:24a00");
	nano::endpoint endpoint;
	ASSERT_TRUE (nano::parse_endpoint (string, endpoint));
}

TEST (parse_endpoint, invalid_address)
{
	std::string string ("::q:24000");
	nano::endpoint endpoint;
	ASSERT_TRUE (nano::parse_endpoint (string, endpoint));
}

TEST (parse_endpoint, no_address)
{
	std::string string (":24000");
	nano::endpoint endpoint;
	ASSERT_TRUE (nano::parse_endpoint (string, endpoint));
}

TEST (parse_endpoint, no_port)
{
	std::string string ("::1:");
	nano::endpoint endpoint;
	ASSERT_TRUE (nano::parse_endpoint (string, endpoint));
}

TEST (parse_endpoint, no_colon)
{
	std::string string ("::1");
	nano::endpoint endpoint;
	ASSERT_TRUE (nano::parse_endpoint (string, endpoint));
}

// If the account doesn't exist, current == end so there's no iteration
TEST (bulk_pull, no_address)
{
	nano::system system (24000, 1);
	auto connection (std::make_shared<nano::bootstrap_server> (nullptr, system.nodes[0]));
	std::unique_ptr<nano::bulk_pull> req (new nano::bulk_pull);
	req->start = 1;
	req->end = 2;
	connection->requests.push (std::unique_ptr<nano::message>{});
	auto request (std::make_shared<nano::bulk_pull_server> (connection, std::move (req)));
	ASSERT_EQ (request->current, request->request->end);
	ASSERT_TRUE (request->current.is_zero ());
}

TEST (bulk_pull, genesis_to_end)
{
	nano::system system (24000, 1);
	auto connection (std::make_shared<nano::bootstrap_server> (nullptr, system.nodes[0]));
	std::unique_ptr<nano::bulk_pull> req (new nano::bulk_pull{});
	req->start = nano::test_genesis_key.pub;
	req->end.clear ();
	connection->requests.push (std::unique_ptr<nano::message>{});
	auto request (std::make_shared<nano::bulk_pull_server> (connection, std::move (req)));
	ASSERT_EQ (system.nodes[0]->latest (nano::test_genesis_key.pub), request->current);
	ASSERT_EQ (request->request->end, request->request->end);
}

// If we can't find the end block, send everything
TEST (bulk_pull, no_end)
{
	nano::system system (24000, 1);
	auto connection (std::make_shared<nano::bootstrap_server> (nullptr, system.nodes[0]));
	std::unique_ptr<nano::bulk_pull> req (new nano::bulk_pull{});
	req->start = nano::test_genesis_key.pub;
	req->end = 1;
	connection->requests.push (std::unique_ptr<nano::message>{});
	auto request (std::make_shared<nano::bulk_pull_server> (connection, std::move (req)));
	ASSERT_EQ (system.nodes[0]->latest (nano::test_genesis_key.pub), request->current);
	ASSERT_TRUE (request->request->end.is_zero ());
}

TEST (bulk_pull, end_not_owned)
{
	nano::system system (24000, 1);
	nano::keypair key2;
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	ASSERT_NE (nullptr, system.wallet (0)->send_action (nano::test_genesis_key.pub, key2.pub, 100));
	nano::block_hash latest (system.nodes[0]->latest (nano::test_genesis_key.pub));
	nano::open_block open (0, 1, 2, nano::keypair ().prv, 4, 5);
	open.hashables.account = key2.pub;
	open.hashables.representative = key2.pub;
	open.hashables.source = latest;
	open.signature = nano::sign_message (key2.prv, key2.pub, open.hash ());
	system.nodes[0]->work_generate_blocking (open);
	ASSERT_EQ (nano::process_result::progress, system.nodes[0]->process (open).code);
	auto connection (std::make_shared<nano::bootstrap_server> (nullptr, system.nodes[0]));
	nano::genesis genesis;
	std::unique_ptr<nano::bulk_pull> req (new nano::bulk_pull{});
	req->start = key2.pub;
	req->end = genesis.hash ();
	connection->requests.push (std::unique_ptr<nano::message>{});
	auto request (std::make_shared<nano::bulk_pull_server> (connection, std::move (req)));
	ASSERT_EQ (request->current, request->request->end);
}

TEST (bulk_pull, none)
{
	nano::system system (24000, 1);
	auto connection (std::make_shared<nano::bootstrap_server> (nullptr, system.nodes[0]));
	nano::genesis genesis;
	std::unique_ptr<nano::bulk_pull> req (new nano::bulk_pull{});
	req->start = nano::test_genesis_key.pub;
	req->end = genesis.hash ();
	connection->requests.push (std::unique_ptr<nano::message>{});
	auto request (std::make_shared<nano::bulk_pull_server> (connection, std::move (req)));
	auto block (request->get_next ());
	ASSERT_EQ (nullptr, block);
}

TEST (bulk_pull, get_next_on_open)
{
	nano::system system (24000, 1);
	auto connection (std::make_shared<nano::bootstrap_server> (nullptr, system.nodes[0]));
	std::unique_ptr<nano::bulk_pull> req (new nano::bulk_pull{});
	req->start = nano::test_genesis_key.pub;
	req->end.clear ();
	connection->requests.push (std::unique_ptr<nano::message>{});
	auto request (std::make_shared<nano::bulk_pull_server> (connection, std::move (req)));
	auto block (request->get_next ());
	ASSERT_NE (nullptr, block);
	ASSERT_TRUE (block->previous ().is_zero ());
	ASSERT_FALSE (connection->requests.empty ());
	ASSERT_EQ (request->current, request->request->end);
}

TEST (bulk_pull, by_block)
{
	nano::system system (24000, 1);
	auto connection (std::make_shared<nano::bootstrap_server> (nullptr, system.nodes[0]));
	nano::genesis genesis;
	std::unique_ptr<nano::bulk_pull> req (new nano::bulk_pull{});
	req->start = genesis.hash ();
	req->end.clear ();
	connection->requests.push (std::unique_ptr<nano::message>{});
	auto request (std::make_shared<nano::bulk_pull_server> (connection, std::move (req)));
	auto block (request->get_next ());
	ASSERT_NE (nullptr, block);
	ASSERT_EQ (block->hash (), genesis.hash ());

	block = request->get_next ();
	ASSERT_EQ (nullptr, block);
}

TEST (bulk_pull, by_block_single)
{
	nano::system system (24000, 1);
	auto connection (std::make_shared<nano::bootstrap_server> (nullptr, system.nodes[0]));
	nano::genesis genesis;
	std::unique_ptr<nano::bulk_pull> req (new nano::bulk_pull{});
	req->start = genesis.hash ();
	req->end = genesis.hash ();
	connection->requests.push (std::unique_ptr<nano::message>{});
	auto request (std::make_shared<nano::bulk_pull_server> (connection, std::move (req)));
	auto block (request->get_next ());
	ASSERT_NE (nullptr, block);
	ASSERT_EQ (block->hash (), genesis.hash ());

	block = request->get_next ();
	ASSERT_EQ (nullptr, block);
}

TEST (bulk_pull, count_limit)
{
	nano::system system (24000, 1);
	nano::genesis genesis;

	auto send1 (std::make_shared<nano::send_block> (system.nodes[0]->latest (nano::test_genesis_key.pub), nano::test_genesis_key.pub, 1, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (system.nodes[0]->latest (nano::test_genesis_key.pub))));
	ASSERT_EQ (nano::process_result::progress, system.nodes[0]->process (*send1).code);
	auto receive1 (std::make_shared<nano::receive_block> (send1->hash (), send1->hash (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (send1->hash ())));
	ASSERT_EQ (nano::process_result::progress, system.nodes[0]->process (*receive1).code);

	auto connection (std::make_shared<nano::bootstrap_server> (nullptr, system.nodes[0]));
	std::unique_ptr<nano::bulk_pull> req (new nano::bulk_pull{});
	req->start = receive1->hash ();
	req->set_count_present (true);
	req->count = 2;
	connection->requests.push (std::unique_ptr<nano::message>{});
	auto request (std::make_shared<nano::bulk_pull_server> (connection, std::move (req)));

	ASSERT_EQ (request->max_count, 2);
	ASSERT_EQ (request->sent_count, 0);

	auto block (request->get_next ());
	ASSERT_EQ (receive1->hash (), block->hash ());

	block = request->get_next ();
	ASSERT_EQ (send1->hash (), block->hash ());

	block = request->get_next ();
	ASSERT_EQ (nullptr, block);
}

TEST (bootstrap_processor, DISABLED_process_none)
{
	nano::system system (24000, 1);
	auto node1 (std::make_shared<nano::node> (system.io_ctx, 24001, nano::unique_path (), system.alarm, system.logging, system.work));
	ASSERT_FALSE (node1->init_error ());
	auto done (false);
	node1->bootstrap_initiator.bootstrap (system.nodes[0]->network.endpoint ());
	while (!done)
	{
		system.io_ctx.run_one ();
	}
	node1->stop ();
}

// Bootstrap can pull one basic block
TEST (bootstrap_processor, process_one)
{
	nano::system system (24000, 1);
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	ASSERT_NE (nullptr, system.wallet (0)->send_action (nano::test_genesis_key.pub, nano::test_genesis_key.pub, 100));
	auto node1 (std::make_shared<nano::node> (system.io_ctx, 24001, nano::unique_path (), system.alarm, system.logging, system.work));
	nano::block_hash hash1 (system.nodes[0]->latest (nano::test_genesis_key.pub));
	nano::block_hash hash2 (node1->latest (nano::test_genesis_key.pub));
	ASSERT_NE (hash1, hash2);
	node1->bootstrap_initiator.bootstrap (system.nodes[0]->network.endpoint ());
	ASSERT_NE (node1->latest (nano::test_genesis_key.pub), system.nodes[0]->latest (nano::test_genesis_key.pub));
	system.deadline_set (10s);
	while (node1->latest (nano::test_genesis_key.pub) != system.nodes[0]->latest (nano::test_genesis_key.pub))
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	ASSERT_EQ (0, node1->active.size ());
	node1->stop ();
}

TEST (bootstrap_processor, process_two)
{
	nano::system system (24000, 1);
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	nano::block_hash hash1 (system.nodes[0]->latest (nano::test_genesis_key.pub));
	ASSERT_NE (nullptr, system.wallet (0)->send_action (nano::test_genesis_key.pub, nano::test_genesis_key.pub, 50));
	nano::block_hash hash2 (system.nodes[0]->latest (nano::test_genesis_key.pub));
	ASSERT_NE (nullptr, system.wallet (0)->send_action (nano::test_genesis_key.pub, nano::test_genesis_key.pub, 50));
	nano::block_hash hash3 (system.nodes[0]->latest (nano::test_genesis_key.pub));
	ASSERT_NE (hash1, hash2);
	ASSERT_NE (hash1, hash3);
	ASSERT_NE (hash2, hash3);
	auto node1 (std::make_shared<nano::node> (system.io_ctx, 24001, nano::unique_path (), system.alarm, system.logging, system.work));
	ASSERT_FALSE (node1->init_error ());
	node1->bootstrap_initiator.bootstrap (system.nodes[0]->network.endpoint ());
	ASSERT_NE (node1->latest (nano::test_genesis_key.pub), system.nodes[0]->latest (nano::test_genesis_key.pub));
	system.deadline_set (10s);
	while (node1->latest (nano::test_genesis_key.pub) != system.nodes[0]->latest (nano::test_genesis_key.pub))
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	node1->stop ();
}

// Bootstrap can pull universal blocks
TEST (bootstrap_processor, process_state)
{
	nano::system system (24000, 1);
	nano::genesis genesis;
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	auto node0 (system.nodes[0]);
	auto block1 (std::make_shared<nano::state_block> (nano::test_genesis_key.pub, node0->latest (nano::test_genesis_key.pub), nano::test_genesis_key.pub, nano::genesis_amount - 100, nano::test_genesis_key.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, 0));
	auto block2 (std::make_shared<nano::state_block> (nano::test_genesis_key.pub, block1->hash (), nano::test_genesis_key.pub, nano::genesis_amount, block1->hash (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, 0));
	node0->work_generate_blocking (*block1);
	node0->work_generate_blocking (*block2);
	node0->process (*block1);
	node0->process (*block2);
	auto node1 (std::make_shared<nano::node> (system.io_ctx, 24001, nano::unique_path (), system.alarm, system.logging, system.work));
	ASSERT_EQ (node0->latest (nano::test_genesis_key.pub), block2->hash ());
	ASSERT_NE (node1->latest (nano::test_genesis_key.pub), block2->hash ());
	node1->bootstrap_initiator.bootstrap (node0->network.endpoint ());
	ASSERT_NE (node1->latest (nano::test_genesis_key.pub), node0->latest (nano::test_genesis_key.pub));
	system.deadline_set (10s);
	while (node1->latest (nano::test_genesis_key.pub) != node0->latest (nano::test_genesis_key.pub))
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	ASSERT_EQ (0, node1->active.size ());
	node1->stop ();
}

TEST (bootstrap_processor, process_new)
{
	nano::system system (24000, 2);
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	nano::keypair key2;
	system.wallet (1)->insert_adhoc (key2.prv);
	ASSERT_NE (nullptr, system.wallet (0)->send_action (nano::test_genesis_key.pub, key2.pub, system.nodes[0]->config.receive_minimum.number ()));
	system.deadline_set (10s);
	while (system.nodes[0]->balance (key2.pub).is_zero ())
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	nano::uint128_t balance1 (system.nodes[0]->balance (nano::test_genesis_key.pub));
	nano::uint128_t balance2 (system.nodes[0]->balance (key2.pub));
	auto node1 (std::make_shared<nano::node> (system.io_ctx, 24002, nano::unique_path (), system.alarm, system.logging, system.work));
	ASSERT_FALSE (node1->init_error ());
	node1->bootstrap_initiator.bootstrap (system.nodes[0]->network.endpoint ());
	system.deadline_set (10s);
	while (node1->balance (key2.pub) != balance2)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	ASSERT_EQ (balance1, node1->balance (nano::test_genesis_key.pub));
	node1->stop ();
}

TEST (bootstrap_processor, pull_diamond)
{
	nano::system system (24000, 1);
	nano::keypair key;
	auto send1 (std::make_shared<nano::send_block> (system.nodes[0]->latest (nano::test_genesis_key.pub), key.pub, 0, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (system.nodes[0]->latest (nano::test_genesis_key.pub))));
	ASSERT_EQ (nano::process_result::progress, system.nodes[0]->process (*send1).code);
	auto open (std::make_shared<nano::open_block> (send1->hash (), 1, key.pub, key.prv, key.pub, system.work.generate (key.pub)));
	ASSERT_EQ (nano::process_result::progress, system.nodes[0]->process (*open).code);
	auto send2 (std::make_shared<nano::send_block> (open->hash (), nano::test_genesis_key.pub, std::numeric_limits<nano::uint128_t>::max () - 100, key.prv, key.pub, system.work.generate (open->hash ())));
	ASSERT_EQ (nano::process_result::progress, system.nodes[0]->process (*send2).code);
	auto receive (std::make_shared<nano::receive_block> (send1->hash (), send2->hash (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (send1->hash ())));
	ASSERT_EQ (nano::process_result::progress, system.nodes[0]->process (*receive).code);
	auto node1 (std::make_shared<nano::node> (system.io_ctx, 24002, nano::unique_path (), system.alarm, system.logging, system.work));
	ASSERT_FALSE (node1->init_error ());
	node1->bootstrap_initiator.bootstrap (system.nodes[0]->network.endpoint ());
	system.deadline_set (10s);
	while (node1->balance (nano::test_genesis_key.pub) != 100)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	ASSERT_EQ (100, node1->balance (nano::test_genesis_key.pub));
	node1->stop ();
}

TEST (bootstrap_processor, push_diamond)
{
	nano::system system (24000, 1);
	nano::keypair key;
	auto node1 (std::make_shared<nano::node> (system.io_ctx, 24002, nano::unique_path (), system.alarm, system.logging, system.work));
	ASSERT_FALSE (node1->init_error ());
	auto wallet1 (node1->wallets.create (100));
	wallet1->insert_adhoc (nano::test_genesis_key.prv);
	wallet1->insert_adhoc (key.prv);
	auto send1 (std::make_shared<nano::send_block> (system.nodes[0]->latest (nano::test_genesis_key.pub), key.pub, 0, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (system.nodes[0]->latest (nano::test_genesis_key.pub))));
	ASSERT_EQ (nano::process_result::progress, node1->process (*send1).code);
	auto open (std::make_shared<nano::open_block> (send1->hash (), 1, key.pub, key.prv, key.pub, system.work.generate (key.pub)));
	ASSERT_EQ (nano::process_result::progress, node1->process (*open).code);
	auto send2 (std::make_shared<nano::send_block> (open->hash (), nano::test_genesis_key.pub, std::numeric_limits<nano::uint128_t>::max () - 100, key.prv, key.pub, system.work.generate (open->hash ())));
	ASSERT_EQ (nano::process_result::progress, node1->process (*send2).code);
	auto receive (std::make_shared<nano::receive_block> (send1->hash (), send2->hash (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (send1->hash ())));
	ASSERT_EQ (nano::process_result::progress, node1->process (*receive).code);
	node1->bootstrap_initiator.bootstrap (system.nodes[0]->network.endpoint ());
	system.deadline_set (10s);
	while (system.nodes[0]->balance (nano::test_genesis_key.pub) != 100)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	ASSERT_EQ (100, system.nodes[0]->balance (nano::test_genesis_key.pub));
	node1->stop ();
}

TEST (bootstrap_processor, push_one)
{
	nano::system system (24000, 1);
	nano::keypair key1;
	auto node1 (std::make_shared<nano::node> (system.io_ctx, 24001, nano::unique_path (), system.alarm, system.logging, system.work));
	auto wallet (node1->wallets.create (nano::uint256_union ()));
	ASSERT_NE (nullptr, wallet);
	wallet->insert_adhoc (nano::test_genesis_key.prv);
	nano::uint128_t balance1 (node1->balance (nano::test_genesis_key.pub));
	ASSERT_NE (nullptr, wallet->send_action (nano::test_genesis_key.pub, key1.pub, 100));
	ASSERT_NE (balance1, node1->balance (nano::test_genesis_key.pub));
	node1->bootstrap_initiator.bootstrap (system.nodes[0]->network.endpoint ());
	system.deadline_set (10s);
	while (system.nodes[0]->balance (nano::test_genesis_key.pub) == balance1)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	node1->stop ();
}

TEST (bootstrap_processor, lazy_hash)
{
	nano::system system (24000, 1);
	nano::genesis genesis;
	nano::keypair key1;
	nano::keypair key2;
	// Generating test chain
	auto send1 (std::make_shared<nano::state_block> (nano::test_genesis_key.pub, genesis.hash (), nano::test_genesis_key.pub, nano::genesis_amount - nano::Gxrb_ratio, key1.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.nodes[0]->work_generate_blocking (genesis.hash ())));
	auto receive1 (std::make_shared<nano::state_block> (key1.pub, 0, key1.pub, nano::Gxrb_ratio, send1->hash (), key1.prv, key1.pub, system.nodes[0]->work_generate_blocking (key1.pub)));
	auto send2 (std::make_shared<nano::state_block> (key1.pub, receive1->hash (), key1.pub, 0, key2.pub, key1.prv, key1.pub, system.nodes[0]->work_generate_blocking (receive1->hash ())));
	auto receive2 (std::make_shared<nano::state_block> (key2.pub, 0, key2.pub, nano::Gxrb_ratio, send2->hash (), key2.prv, key2.pub, system.nodes[0]->work_generate_blocking (key2.pub)));
	// Processing test chain
	system.nodes[0]->block_processor.add (send1);
	system.nodes[0]->block_processor.add (receive1);
	system.nodes[0]->block_processor.add (send2);
	system.nodes[0]->block_processor.add (receive2);
	system.nodes[0]->block_processor.flush ();
	// Start lazy bootstrap with last block in chain known
	auto node1 (std::make_shared<nano::node> (system.io_ctx, 24001, nano::unique_path (), system.alarm, system.logging, system.work));
	node1->network.udp_channels.insert (system.nodes[0]->network.endpoint (), node1->network_params.protocol.protocol_version);
	node1->bootstrap_initiator.bootstrap_lazy (receive2->hash ());
	// Check processed blocks
	system.deadline_set (10s);
	while (node1->balance (key2.pub) == 0)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	node1->stop ();
}

TEST (bootstrap_processor, lazy_max_pull_count)
{
	nano::system system (24000, 1);
	nano::genesis genesis;
	nano::keypair key1;
	nano::keypair key2;
	// Generating test chain
	auto send1 (std::make_shared<nano::state_block> (nano::test_genesis_key.pub, genesis.hash (), nano::test_genesis_key.pub, nano::genesis_amount - nano::Gxrb_ratio, key1.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.nodes[0]->work_generate_blocking (genesis.hash ())));
	auto receive1 (std::make_shared<nano::state_block> (key1.pub, 0, key1.pub, nano::Gxrb_ratio, send1->hash (), key1.prv, key1.pub, system.nodes[0]->work_generate_blocking (key1.pub)));
	auto send2 (std::make_shared<nano::state_block> (key1.pub, receive1->hash (), key1.pub, 0, key2.pub, key1.prv, key1.pub, system.nodes[0]->work_generate_blocking (receive1->hash ())));
	auto receive2 (std::make_shared<nano::state_block> (key2.pub, 0, key2.pub, nano::Gxrb_ratio, send2->hash (), key2.prv, key2.pub, system.nodes[0]->work_generate_blocking (key2.pub)));
	auto change1 (std::make_shared<nano::state_block> (key2.pub, receive2->hash (), key1.pub, nano::Gxrb_ratio, 0, key2.prv, key2.pub, system.nodes[0]->work_generate_blocking (receive2->hash ())));
	auto change2 (std::make_shared<nano::state_block> (key2.pub, change1->hash (), nano::test_genesis_key.pub, nano::Gxrb_ratio, 0, key2.prv, key2.pub, system.nodes[0]->work_generate_blocking (change1->hash ())));
	auto change3 (std::make_shared<nano::state_block> (key2.pub, change2->hash (), key2.pub, nano::Gxrb_ratio, 0, key2.prv, key2.pub, system.nodes[0]->work_generate_blocking (change2->hash ())));
	// Processing test chain
	system.nodes[0]->block_processor.add (send1);
	system.nodes[0]->block_processor.add (receive1);
	system.nodes[0]->block_processor.add (send2);
	system.nodes[0]->block_processor.add (receive2);
	system.nodes[0]->block_processor.add (change1);
	system.nodes[0]->block_processor.add (change2);
	system.nodes[0]->block_processor.add (change3);
	system.nodes[0]->block_processor.flush ();
	// Start lazy bootstrap with last block in chain known
	auto node1 (std::make_shared<nano::node> (system.io_ctx, 24001, nano::unique_path (), system.alarm, system.logging, system.work));
	node1->network.udp_channels.insert (system.nodes[0]->network.endpoint (), node1->network_params.protocol.protocol_version);
	node1->bootstrap_initiator.bootstrap_lazy (change3->hash ());
	// Check processed blocks
	system.deadline_set (10s);
	while (node1->block (change3->hash ()) == nullptr)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	node1->stop ();
}

TEST (bootstrap_processor, wallet_lazy_frontier)
{
	nano::system system (24000, 1);
	nano::genesis genesis;
	nano::keypair key1;
	nano::keypair key2;
	// Generating test chain
	auto send1 (std::make_shared<nano::state_block> (nano::test_genesis_key.pub, genesis.hash (), nano::test_genesis_key.pub, nano::genesis_amount - nano::Gxrb_ratio, key1.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.nodes[0]->work_generate_blocking (genesis.hash ())));
	auto receive1 (std::make_shared<nano::state_block> (key1.pub, 0, key1.pub, nano::Gxrb_ratio, send1->hash (), key1.prv, key1.pub, system.nodes[0]->work_generate_blocking (key1.pub)));
	auto send2 (std::make_shared<nano::state_block> (key1.pub, receive1->hash (), key1.pub, 0, key2.pub, key1.prv, key1.pub, system.nodes[0]->work_generate_blocking (receive1->hash ())));
	auto receive2 (std::make_shared<nano::state_block> (key2.pub, 0, key2.pub, nano::Gxrb_ratio, send2->hash (), key2.prv, key2.pub, system.nodes[0]->work_generate_blocking (key2.pub)));
	// Processing test chain
	system.nodes[0]->block_processor.add (send1);
	system.nodes[0]->block_processor.add (receive1);
	system.nodes[0]->block_processor.add (send2);
	system.nodes[0]->block_processor.add (receive2);
	system.nodes[0]->block_processor.flush ();
	// Start wallet lazy bootstrap
	auto node1 (std::make_shared<nano::node> (system.io_ctx, 24001, nano::unique_path (), system.alarm, system.logging, system.work));
	node1->network.udp_channels.insert (system.nodes[0]->network.endpoint (), node1->network_params.protocol.protocol_version);
	auto wallet (node1->wallets.create (nano::uint256_union ()));
	ASSERT_NE (nullptr, wallet);
	wallet->insert_adhoc (key2.prv);
	node1->bootstrap_wallet ();
	// Check processed blocks
	system.deadline_set (10s);
	while (!node1->ledger.block_exists (receive2->hash ()))
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	node1->stop ();
}

TEST (bootstrap_processor, wallet_lazy_pending)
{
	nano::system system (24000, 1);
	nano::genesis genesis;
	nano::keypair key1;
	nano::keypair key2;
	// Generating test chain
	auto send1 (std::make_shared<nano::state_block> (nano::test_genesis_key.pub, genesis.hash (), nano::test_genesis_key.pub, nano::genesis_amount - nano::Gxrb_ratio, key1.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.nodes[0]->work_generate_blocking (genesis.hash ())));
	auto receive1 (std::make_shared<nano::state_block> (key1.pub, 0, key1.pub, nano::Gxrb_ratio, send1->hash (), key1.prv, key1.pub, system.nodes[0]->work_generate_blocking (key1.pub)));
	auto send2 (std::make_shared<nano::state_block> (key1.pub, receive1->hash (), key1.pub, 0, key2.pub, key1.prv, key1.pub, system.nodes[0]->work_generate_blocking (receive1->hash ())));
	// Processing test chain
	system.nodes[0]->block_processor.add (send1);
	system.nodes[0]->block_processor.add (receive1);
	system.nodes[0]->block_processor.add (send2);
	system.nodes[0]->block_processor.flush ();
	// Start wallet lazy bootstrap
	auto node1 (std::make_shared<nano::node> (system.io_ctx, 24001, nano::unique_path (), system.alarm, system.logging, system.work));
	node1->network.udp_channels.insert (system.nodes[0]->network.endpoint (), node1->network_params.protocol.protocol_version);
	auto wallet (node1->wallets.create (nano::uint256_union ()));
	ASSERT_NE (nullptr, wallet);
	wallet->insert_adhoc (key2.prv);
	node1->bootstrap_wallet ();
	// Check processed blocks
	system.deadline_set (10s);
	while (!node1->ledger.block_exists (send2->hash ()))
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	node1->stop ();
}

TEST (frontier_req_response, DISABLED_destruction)
{
	{
		std::shared_ptr<nano::frontier_req_server> hold; // Destructing tcp acceptor on non-existent io_context
		{
			nano::system system (24000, 1);
			auto connection (std::make_shared<nano::bootstrap_server> (nullptr, system.nodes[0]));
			std::unique_ptr<nano::frontier_req> req (new nano::frontier_req);
			req->start.clear ();
			req->age = std::numeric_limits<decltype (req->age)>::max ();
			req->count = std::numeric_limits<decltype (req->count)>::max ();
			connection->requests.push (std::unique_ptr<nano::message>{});
			hold = std::make_shared<nano::frontier_req_server> (connection, std::move (req));
		}
	}
	ASSERT_TRUE (true);
}

TEST (frontier_req, begin)
{
	nano::system system (24000, 1);
	auto connection (std::make_shared<nano::bootstrap_server> (nullptr, system.nodes[0]));
	std::unique_ptr<nano::frontier_req> req (new nano::frontier_req);
	req->start.clear ();
	req->age = std::numeric_limits<decltype (req->age)>::max ();
	req->count = std::numeric_limits<decltype (req->count)>::max ();
	connection->requests.push (std::unique_ptr<nano::message>{});
	auto request (std::make_shared<nano::frontier_req_server> (connection, std::move (req)));
	ASSERT_EQ (nano::test_genesis_key.pub, request->current);
	nano::genesis genesis;
	ASSERT_EQ (genesis.hash (), request->frontier);
}

TEST (frontier_req, end)
{
	nano::system system (24000, 1);
	auto connection (std::make_shared<nano::bootstrap_server> (nullptr, system.nodes[0]));
	std::unique_ptr<nano::frontier_req> req (new nano::frontier_req);
	req->start = nano::test_genesis_key.pub.number () + 1;
	req->age = std::numeric_limits<decltype (req->age)>::max ();
	req->count = std::numeric_limits<decltype (req->count)>::max ();
	connection->requests.push (std::unique_ptr<nano::message>{});
	auto request (std::make_shared<nano::frontier_req_server> (connection, std::move (req)));
	ASSERT_TRUE (request->current.is_zero ());
}

TEST (frontier_req, count)
{
	nano::system system (24000, 1);
	auto & node1 (*system.nodes[0]);
	nano::genesis genesis;
	// Public key FB93... after genesis in accounts table
	nano::keypair key1 ("ED5AE0A6505B14B67435C29FD9FEEBC26F597D147BC92F6D795FFAD7AFD3D967");
	nano::state_block send1 (nano::test_genesis_key.pub, genesis.hash (), nano::test_genesis_key.pub, nano::genesis_amount - nano::Gxrb_ratio, key1.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, 0);
	node1.work_generate_blocking (send1);
	ASSERT_EQ (nano::process_result::progress, node1.process (send1).code);
	nano::state_block receive1 (key1.pub, 0, nano::test_genesis_key.pub, nano::Gxrb_ratio, send1.hash (), key1.prv, key1.pub, 0);
	node1.work_generate_blocking (receive1);
	ASSERT_EQ (nano::process_result::progress, node1.process (receive1).code);
	auto connection (std::make_shared<nano::bootstrap_server> (nullptr, system.nodes[0]));
	std::unique_ptr<nano::frontier_req> req (new nano::frontier_req);
	req->start.clear ();
	req->age = std::numeric_limits<decltype (req->age)>::max ();
	req->count = 1;
	connection->requests.push (std::unique_ptr<nano::message>{});
	auto request (std::make_shared<nano::frontier_req_server> (connection, std::move (req)));
	ASSERT_EQ (nano::test_genesis_key.pub, request->current);
	ASSERT_EQ (send1.hash (), request->frontier);
}

TEST (frontier_req, time_bound)
{
	nano::system system (24000, 1);
	auto connection (std::make_shared<nano::bootstrap_server> (nullptr, system.nodes[0]));
	std::unique_ptr<nano::frontier_req> req (new nano::frontier_req);
	req->start.clear ();
	req->age = 1;
	req->count = std::numeric_limits<decltype (req->count)>::max ();
	connection->requests.push (std::unique_ptr<nano::message>{});
	auto request (std::make_shared<nano::frontier_req_server> (connection, std::move (req)));
	ASSERT_EQ (nano::test_genesis_key.pub, request->current);
	// Wait 2 seconds until age of account will be > 1 seconds
	std::this_thread::sleep_for (std::chrono::milliseconds (2100));
	std::unique_ptr<nano::frontier_req> req2 (new nano::frontier_req);
	req2->start.clear ();
	req2->age = 1;
	req2->count = std::numeric_limits<decltype (req2->count)>::max ();
	auto connection2 (std::make_shared<nano::bootstrap_server> (nullptr, system.nodes[0]));
	connection2->requests.push (std::unique_ptr<nano::message>{});
	auto request2 (std::make_shared<nano::frontier_req_server> (connection, std::move (req2)));
	ASSERT_TRUE (request2->current.is_zero ());
}

TEST (frontier_req, time_cutoff)
{
	nano::system system (24000, 1);
	auto connection (std::make_shared<nano::bootstrap_server> (nullptr, system.nodes[0]));
	std::unique_ptr<nano::frontier_req> req (new nano::frontier_req);
	req->start.clear ();
	req->age = 3;
	req->count = std::numeric_limits<decltype (req->count)>::max ();
	connection->requests.push (std::unique_ptr<nano::message>{});
	auto request (std::make_shared<nano::frontier_req_server> (connection, std::move (req)));
	ASSERT_EQ (nano::test_genesis_key.pub, request->current);
	nano::genesis genesis;
	ASSERT_EQ (genesis.hash (), request->frontier);
	// Wait 4 seconds until age of account will be > 3 seconds
	std::this_thread::sleep_for (std::chrono::milliseconds (4100));
	std::unique_ptr<nano::frontier_req> req2 (new nano::frontier_req);
	req2->start.clear ();
	req2->age = 3;
	req2->count = std::numeric_limits<decltype (req2->count)>::max ();
	auto connection2 (std::make_shared<nano::bootstrap_server> (nullptr, system.nodes[0]));
	connection2->requests.push (std::unique_ptr<nano::message>{});
	auto request2 (std::make_shared<nano::frontier_req_server> (connection, std::move (req2)));
	ASSERT_TRUE (request2->frontier.is_zero ());
}

TEST (bulk, genesis)
{
	nano::system system (24000, 1);
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	auto node1 (std::make_shared<nano::node> (system.io_ctx, 24001, nano::unique_path (), system.alarm, system.logging, system.work));
	ASSERT_FALSE (node1->init_error ());
	nano::block_hash latest1 (system.nodes[0]->latest (nano::test_genesis_key.pub));
	nano::block_hash latest2 (node1->latest (nano::test_genesis_key.pub));
	ASSERT_EQ (latest1, latest2);
	nano::keypair key2;
	ASSERT_NE (nullptr, system.wallet (0)->send_action (nano::test_genesis_key.pub, key2.pub, 100));
	nano::block_hash latest3 (system.nodes[0]->latest (nano::test_genesis_key.pub));
	ASSERT_NE (latest1, latest3);
	node1->bootstrap_initiator.bootstrap (system.nodes[0]->network.endpoint ());
	system.deadline_set (10s);
	while (node1->latest (nano::test_genesis_key.pub) != system.nodes[0]->latest (nano::test_genesis_key.pub))
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	ASSERT_EQ (node1->latest (nano::test_genesis_key.pub), system.nodes[0]->latest (nano::test_genesis_key.pub));
	node1->stop ();
}

TEST (bulk, offline_send)
{
	nano::system system (24000, 1);
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	auto node1 (std::make_shared<nano::node> (system.io_ctx, 24001, nano::unique_path (), system.alarm, system.logging, system.work));
	ASSERT_FALSE (node1->init_error ());
	node1->start ();
	system.nodes.push_back (node1);
	nano::keypair key2;
	auto wallet (node1->wallets.create (nano::uint256_union ()));
	wallet->insert_adhoc (key2.prv);
	ASSERT_NE (nullptr, system.wallet (0)->send_action (nano::test_genesis_key.pub, key2.pub, system.nodes[0]->config.receive_minimum.number ()));
	ASSERT_NE (std::numeric_limits<nano::uint256_t>::max (), system.nodes[0]->balance (nano::test_genesis_key.pub));
	// Wait to finish election background tasks
	system.deadline_set (10s);
	while (!system.nodes[0]->active.empty ())
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	// Initiate bootstrap
	node1->bootstrap_initiator.bootstrap (system.nodes[0]->network.endpoint ());
	// Nodes should find each other
	do
	{
		ASSERT_NO_ERROR (system.poll ());
	} while (system.nodes[0]->network.empty () || node1->network.empty ());
	// Send block arrival via bootstrap
	while (node1->balance (nano::test_genesis_key.pub) == std::numeric_limits<nano::uint256_t>::max ())
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	// Receiving send block
	system.deadline_set (20s);
	while (node1->balance (key2.pub) != system.nodes[0]->config.receive_minimum.number ())
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	node1->stop ();
}

TEST (network, ipv6)
{
	boost::asio::ip::address_v6 address (boost::asio::ip::address_v6::from_string ("::ffff:127.0.0.1"));
	ASSERT_TRUE (address.is_v4_mapped ());
	nano::endpoint endpoint1 (address, 16384);
	std::vector<uint8_t> bytes1;
	{
		nano::vectorstream stream (bytes1);
		nano::write (stream, address.to_bytes ());
	}
	ASSERT_EQ (16, bytes1.size ());
	for (auto i (bytes1.begin ()), n (bytes1.begin () + 10); i != n; ++i)
	{
		ASSERT_EQ (0, *i);
	}
	ASSERT_EQ (0xff, bytes1[10]);
	ASSERT_EQ (0xff, bytes1[11]);
	std::array<uint8_t, 16> bytes2;
	nano::bufferstream stream (bytes1.data (), bytes1.size ());
	auto error (nano::try_read (stream, bytes2));
	ASSERT_FALSE (error);
	nano::endpoint endpoint2 (boost::asio::ip::address_v6 (bytes2), 16384);
	ASSERT_EQ (endpoint1, endpoint2);
}

TEST (network, ipv6_from_ipv4)
{
	nano::endpoint endpoint1 (boost::asio::ip::address_v4::loopback (), 16000);
	ASSERT_TRUE (endpoint1.address ().is_v4 ());
	nano::endpoint endpoint2 (boost::asio::ip::address_v6::v4_mapped (endpoint1.address ().to_v4 ()), 16000);
	ASSERT_TRUE (endpoint2.address ().is_v6 ());
}

TEST (network, ipv6_bind_send_ipv4)
{
	boost::asio::io_context io_ctx;
	nano::endpoint endpoint1 (boost::asio::ip::address_v6::any (), 24000);
	nano::endpoint endpoint2 (boost::asio::ip::address_v4::any (), 24001);
	std::array<uint8_t, 16> bytes1;
	auto finish1 (false);
	nano::endpoint endpoint3;
	boost::asio::ip::udp::socket socket1 (io_ctx, endpoint1);
	socket1.async_receive_from (boost::asio::buffer (bytes1.data (), bytes1.size ()), endpoint3, [&finish1](boost::system::error_code const & error, size_t size_a) {
		ASSERT_FALSE (error);
		ASSERT_EQ (16, size_a);
		finish1 = true;
	});
	boost::asio::ip::udp::socket socket2 (io_ctx, endpoint2);
	nano::endpoint endpoint5 (boost::asio::ip::address_v4::loopback (), 24000);
	nano::endpoint endpoint6 (boost::asio::ip::address_v6::v4_mapped (boost::asio::ip::address_v4::loopback ()), 24001);
	socket2.async_send_to (boost::asio::buffer (std::array<uint8_t, 16>{}, 16), endpoint5, [](boost::system::error_code const & error, size_t size_a) {
		ASSERT_FALSE (error);
		ASSERT_EQ (16, size_a);
	});
	auto iterations (0);
	while (!finish1)
	{
		io_ctx.poll ();
		++iterations;
		ASSERT_LT (iterations, 200);
	}
	ASSERT_EQ (endpoint6, endpoint3);
	std::array<uint8_t, 16> bytes2;
	nano::endpoint endpoint4;
	socket2.async_receive_from (boost::asio::buffer (bytes2.data (), bytes2.size ()), endpoint4, [](boost::system::error_code const & error, size_t size_a) {
		ASSERT_FALSE (!error);
		ASSERT_EQ (16, size_a);
	});
	socket1.async_send_to (boost::asio::buffer (std::array<uint8_t, 16>{}, 16), endpoint6, [](boost::system::error_code const & error, size_t size_a) {
		ASSERT_FALSE (error);
		ASSERT_EQ (16, size_a);
	});
}

TEST (network, endpoint_bad_fd)
{
	nano::system system (24000, 1);
	system.nodes[0]->stop ();
	auto endpoint (system.nodes[0]->network.endpoint ());
	ASSERT_TRUE (endpoint.address ().is_loopback ());
	// The endpoint is invalidated asynchronously
	system.deadline_set (10s);
	while (system.nodes[0]->network.endpoint ().port () != 0)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
}

TEST (network, reserved_address)
{
	nano::system system (24000, 1);
	// 0 port test
	ASSERT_TRUE (nano::transport::reserved_address (nano::endpoint (boost::asio::ip::address_v6::from_string ("2001::"), 0)));
	// Valid address test
	ASSERT_FALSE (nano::transport::reserved_address (nano::endpoint (boost::asio::ip::address_v6::from_string ("2001::"), 1)));
	nano::endpoint loopback (boost::asio::ip::address_v6::from_string ("::1"), 1);
	ASSERT_FALSE (nano::transport::reserved_address (loopback));
	nano::endpoint private_network_peer (boost::asio::ip::address_v6::from_string ("::ffff:10.0.0.0"), 1);
	ASSERT_TRUE (nano::transport::reserved_address (private_network_peer, false));
	ASSERT_FALSE (nano::transport::reserved_address (private_network_peer, true));
}

TEST (node, port_mapping)
{
	nano::system system (24000, 1);
	auto node0 (system.nodes[0]);
	node0->port_mapping.refresh_devices ();
	node0->port_mapping.start ();
	auto end (std::chrono::steady_clock::now () + std::chrono::seconds (500));
	(void)end;
	//while (std::chrono::steady_clock::now () < end)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
}

TEST (message_buffer_manager, one_buffer)
{
	nano::stat stats;
	nano::message_buffer_manager buffer (stats, 512, 1);
	auto buffer1 (buffer.allocate ());
	ASSERT_NE (nullptr, buffer1);
	buffer.enqueue (buffer1);
	auto buffer2 (buffer.dequeue ());
	ASSERT_EQ (buffer1, buffer2);
	buffer.release (buffer2);
	auto buffer3 (buffer.allocate ());
	ASSERT_EQ (buffer1, buffer3);
}

TEST (message_buffer_manager, two_buffers)
{
	nano::stat stats;
	nano::message_buffer_manager buffer (stats, 512, 2);
	auto buffer1 (buffer.allocate ());
	ASSERT_NE (nullptr, buffer1);
	auto buffer2 (buffer.allocate ());
	ASSERT_NE (nullptr, buffer2);
	ASSERT_NE (buffer1, buffer2);
	buffer.enqueue (buffer2);
	buffer.enqueue (buffer1);
	auto buffer3 (buffer.dequeue ());
	ASSERT_EQ (buffer2, buffer3);
	auto buffer4 (buffer.dequeue ());
	ASSERT_EQ (buffer1, buffer4);
	buffer.release (buffer3);
	buffer.release (buffer4);
	auto buffer5 (buffer.allocate ());
	ASSERT_EQ (buffer2, buffer5);
	auto buffer6 (buffer.allocate ());
	ASSERT_EQ (buffer1, buffer6);
}

TEST (message_buffer_manager, one_overflow)
{
	nano::stat stats;
	nano::message_buffer_manager buffer (stats, 512, 1);
	auto buffer1 (buffer.allocate ());
	ASSERT_NE (nullptr, buffer1);
	buffer.enqueue (buffer1);
	auto buffer2 (buffer.allocate ());
	ASSERT_EQ (buffer1, buffer2);
}

TEST (message_buffer_manager, two_overflow)
{
	nano::stat stats;
	nano::message_buffer_manager buffer (stats, 512, 2);
	auto buffer1 (buffer.allocate ());
	ASSERT_NE (nullptr, buffer1);
	buffer.enqueue (buffer1);
	auto buffer2 (buffer.allocate ());
	ASSERT_NE (nullptr, buffer2);
	ASSERT_NE (buffer1, buffer2);
	buffer.enqueue (buffer2);
	auto buffer3 (buffer.allocate ());
	ASSERT_EQ (buffer1, buffer3);
	auto buffer4 (buffer.allocate ());
	ASSERT_EQ (buffer2, buffer4);
}

TEST (message_buffer_manager, one_buffer_multithreaded)
{
	nano::stat stats;
	nano::message_buffer_manager buffer (stats, 512, 1);
	boost::thread thread ([&buffer]() {
		auto done (false);
		while (!done)
		{
			auto item (buffer.dequeue ());
			done = item == nullptr;
			if (item != nullptr)
			{
				buffer.release (item);
			}
		}
	});
	auto buffer1 (buffer.allocate ());
	ASSERT_NE (nullptr, buffer1);
	buffer.enqueue (buffer1);
	auto buffer2 (buffer.allocate ());
	ASSERT_EQ (buffer1, buffer2);
	buffer.stop ();
	thread.join ();
}

TEST (message_buffer_manager, many_buffers_multithreaded)
{
	nano::stat stats;
	nano::message_buffer_manager buffer (stats, 512, 16);
	std::vector<boost::thread> threads;
	for (auto i (0); i < 4; ++i)
	{
		threads.push_back (boost::thread ([&buffer]() {
			auto done (false);
			while (!done)
			{
				auto item (buffer.dequeue ());
				done = item == nullptr;
				if (item != nullptr)
				{
					buffer.release (item);
				}
			}
		}));
	}
	std::atomic_int count (0);
	for (auto i (0); i < 4; ++i)
	{
		threads.push_back (boost::thread ([&buffer, &count]() {
			auto done (false);
			for (auto i (0); !done && i < 1000; ++i)
			{
				auto item (buffer.allocate ());
				done = item == nullptr;
				if (item != nullptr)
				{
					buffer.enqueue (item);
					++count;
					if (count > 3000)
					{
						buffer.stop ();
					}
				}
			}
		}));
	}
	buffer.stop ();
	for (auto & i : threads)
	{
		i.join ();
	}
}

TEST (message_buffer_manager, stats)
{
	nano::stat stats;
	nano::message_buffer_manager buffer (stats, 512, 1);
	auto buffer1 (buffer.allocate ());
	buffer.enqueue (buffer1);
	buffer.allocate ();
	ASSERT_EQ (1, stats.count (nano::stat::type::udp, nano::stat::detail::overflow));
}

TEST (bulk_pull_account, basics)
{
	nano::system system (24000, 1);
	system.nodes[0]->config.receive_minimum = nano::uint128_union (20);
	nano::keypair key1;
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	system.wallet (0)->insert_adhoc (key1.prv);
	auto send1 (system.wallet (0)->send_action (nano::genesis_account, key1.pub, 25));
	auto send2 (system.wallet (0)->send_action (nano::genesis_account, key1.pub, 10));
	auto send3 (system.wallet (0)->send_action (nano::genesis_account, key1.pub, 2));
	system.deadline_set (5s);
	while (system.nodes[0]->balance (key1.pub) != 25)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	auto connection (std::make_shared<nano::bootstrap_server> (nullptr, system.nodes[0]));

	{
		std::unique_ptr<nano::bulk_pull_account> req (new nano::bulk_pull_account{});
		req->account = key1.pub;
		req->minimum_amount = 5;
		req->flags = nano::bulk_pull_account_flags ();
		connection->requests.push (std::unique_ptr<nano::message>{});
		auto request (std::make_shared<nano::bulk_pull_account_server> (connection, std::move (req)));
		ASSERT_FALSE (request->invalid_request);
		ASSERT_FALSE (request->pending_include_address);
		ASSERT_FALSE (request->pending_address_only);
		ASSERT_EQ (request->current_key.account, key1.pub);
		ASSERT_EQ (request->current_key.hash, 0);
		auto block_data (request->get_next ());
		ASSERT_EQ (send2->hash (), block_data.first.get ()->hash);
		ASSERT_EQ (nano::uint128_union (10), block_data.second.get ()->amount);
		ASSERT_EQ (nano::genesis_account, block_data.second.get ()->source);
		ASSERT_EQ (nullptr, request->get_next ().first.get ());
	}

	{
		std::unique_ptr<nano::bulk_pull_account> req (new nano::bulk_pull_account{});
		req->account = key1.pub;
		req->minimum_amount = 0;
		req->flags = nano::bulk_pull_account_flags::pending_address_only;
		auto request (std::make_shared<nano::bulk_pull_account_server> (connection, std::move (req)));
		ASSERT_TRUE (request->pending_address_only);
		auto block_data (request->get_next ());
		ASSERT_NE (nullptr, block_data.first.get ());
		ASSERT_NE (nullptr, block_data.second.get ());
		ASSERT_EQ (nano::genesis_account, block_data.second.get ()->source);
		block_data = request->get_next ();
		ASSERT_EQ (nullptr, block_data.first.get ());
		ASSERT_EQ (nullptr, block_data.second.get ());
	}
}

TEST (bootstrap, tcp_node_id_handshake)
{
	nano::system system (24000, 1);
	auto socket (std::make_shared<nano::socket> (system.nodes[0]));
	auto bootstrap_endpoint (system.nodes[0]->bootstrap.endpoint ());
	auto cookie (system.nodes[0]->network.syn_cookies.assign (nano::transport::map_tcp_to_endpoint (bootstrap_endpoint)));
	nano::node_id_handshake node_id_handshake (cookie, boost::none);
	auto input (node_id_handshake.to_bytes ());
	std::atomic<bool> write_done (false);
	socket->async_connect (bootstrap_endpoint, [&input, socket, &write_done](boost::system::error_code const & ec) {
		ASSERT_FALSE (ec);
		socket->async_write (input, [&input, &write_done](boost::system::error_code const & ec, size_t size_a) {
			ASSERT_FALSE (ec);
			ASSERT_EQ (input->size (), size_a);
			write_done = true;
		});
	});

	system.deadline_set (std::chrono::seconds (5));
	while (!write_done)
	{
		ASSERT_NO_ERROR (system.poll ());
	}

	boost::optional<std::pair<nano::account, nano::signature>> response_zero (std::make_pair (nano::account (0), nano::signature (0)));
	nano::node_id_handshake node_id_handshake_response (boost::none, response_zero);
	auto output (node_id_handshake_response.to_bytes ());
	std::atomic<bool> done (false);
	socket->async_read (output, output->size (), [&output, &done](boost::system::error_code const & ec, size_t size_a) {
		ASSERT_FALSE (ec);
		ASSERT_EQ (output->size (), size_a);
		done = true;
	});
	system.deadline_set (std::chrono::seconds (5));
	while (!done)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
}

namespace
{
void add_callback_stats (nano::node & node)
{
	node.observers.blocks.add ([& stats = node.stats](nano::election_status const & status_a, nano::account const &, nano::amount const &, bool) {
		stats.inc (nano::stat::type::http_callback, nano::stat::detail::http_callback, nano::stat::dir::out);
	});
}
}

TEST (confirmation_height, single)
{
	auto amount (std::numeric_limits<nano::uint128_t>::max ());
	nano::system system (24000, 2);
	nano::keypair key1;
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	nano::block_hash latest1 (system.nodes[0]->latest (nano::test_genesis_key.pub));
	system.wallet (1)->insert_adhoc (key1.prv);
	auto send1 (std::make_shared<nano::send_block> (latest1, key1.pub, amount - system.nodes[0]->config.receive_minimum.number (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (latest1)));

	// Check confirmation heights before, should be uninitialized (1 for genesis).
	uint64_t confirmation_height;
	for (auto & node : system.nodes)
	{
		add_callback_stats (*node);
		auto transaction = node->store.tx_begin_read ();
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, nano::test_genesis_key.pub, confirmation_height));
		ASSERT_EQ (1, confirmation_height);
	}

	for (auto & node : system.nodes)
	{
		node->process_active (send1);
		node->block_processor.flush ();

		system.deadline_set (10s);
		while (true)
		{
			auto transaction = node->store.tx_begin_read ();
			if (node->ledger.block_confirmed (transaction, send1->hash ()))
			{
				break;
			}

			ASSERT_NO_ERROR (system.poll ());
		}

		auto transaction = node->store.tx_begin_write ();
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, nano::test_genesis_key.pub, confirmation_height));
		ASSERT_EQ (2, confirmation_height);

		// Rollbacks should fail as these blocks have been cemented
		ASSERT_TRUE (node->ledger.rollback (transaction, latest1));
		ASSERT_TRUE (node->ledger.rollback (transaction, send1->hash ()));
		ASSERT_EQ (1, node->stats.count (nano::stat::type::confirmation_height, nano::stat::detail::blocks_confirmed, nano::stat::dir::in));
		ASSERT_EQ (1, node->stats.count (nano::stat::type::http_callback, nano::stat::detail::http_callback, nano::stat::dir::out));
	}
}

TEST (confirmation_height, multiple_accounts)
{
	nano::system system;
	nano::node_config node_config (24001, system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	system.add_node (node_config);
	node_config.peering_port = 24002;
	system.add_node (node_config);
	nano::keypair key1;
	nano::keypair key2;
	nano::keypair key3;
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	nano::block_hash latest1 (system.nodes[0]->latest (nano::test_genesis_key.pub));
	system.wallet (1)->insert_adhoc (key1.prv);
	system.wallet (0)->insert_adhoc (key2.prv);
	system.wallet (1)->insert_adhoc (key3.prv);

	// Send to all accounts
	nano::send_block send1 (latest1, key1.pub, system.nodes.front ()->config.online_weight_minimum.number () + 300, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (latest1));
	nano::send_block send2 (send1.hash (), key2.pub, system.nodes.front ()->config.online_weight_minimum.number () + 200, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (send1.hash ()));
	nano::send_block send3 (send2.hash (), key3.pub, system.nodes.front ()->config.online_weight_minimum.number () + 100, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (send2.hash ()));

	// Open all accounts
	nano::open_block open1 (send1.hash (), nano::genesis_account, key1.pub, key1.prv, key1.pub, system.work.generate (key1.pub));
	nano::open_block open2 (send2.hash (), nano::genesis_account, key2.pub, key2.prv, key2.pub, system.work.generate (key2.pub));
	nano::open_block open3 (send3.hash (), nano::genesis_account, key3.pub, key3.prv, key3.pub, system.work.generate (key3.pub));

	// Send and recieve various blocks to these accounts
	nano::send_block send4 (open1.hash (), key2.pub, 50, key1.prv, key1.pub, system.work.generate (open1.hash ()));
	nano::send_block send5 (send4.hash (), key2.pub, 10, key1.prv, key1.pub, system.work.generate (send4.hash ()));

	nano::receive_block receive1 (open2.hash (), send4.hash (), key2.prv, key2.pub, system.work.generate (open2.hash ()));
	nano::send_block send6 (receive1.hash (), key3.pub, 10, key2.prv, key2.pub, system.work.generate (receive1.hash ()));
	nano::receive_block receive2 (send6.hash (), send5.hash (), key2.prv, key2.pub, system.work.generate (send6.hash ()));

	for (auto & node : system.nodes)
	{
		add_callback_stats (*node);

		auto transaction = node->store.tx_begin_write ();
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send1).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send2).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send3).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, open1).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, open2).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, open3).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send4).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send5).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, receive1).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send6).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, receive2).code);

		// Check confirmation heights of all the accounts are uninitialized (0),
		// as we have any just added them to the ledger and not processed any live transactions yet.
		uint64_t confirmation_height;
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, nano::test_genesis_key.pub, confirmation_height));
		ASSERT_EQ (1, confirmation_height);
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, key1.pub, confirmation_height));
		ASSERT_EQ (0, confirmation_height);
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, key2.pub, confirmation_height));
		ASSERT_EQ (0, confirmation_height);
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, key3.pub, confirmation_height));
		ASSERT_EQ (0, confirmation_height);
	}

	// The nodes process a live receive which propagates across to all accounts
	auto receive3 = std::make_shared<nano::receive_block> (open3.hash (), send6.hash (), key3.prv, key3.pub, system.work.generate (open3.hash ()));

	for (auto & node : system.nodes)
	{
		node->process_active (receive3);
		node->block_processor.flush ();

		system.deadline_set (10s);
		while (true)
		{
			auto transaction = node->store.tx_begin_read ();
			if (node->ledger.block_confirmed (transaction, receive3->hash ()))
			{
				break;
			}

			ASSERT_NO_ERROR (system.poll ());
		}

		nano::account_info account_info;
		uint64_t confirmation_height;
		auto & store = node->store;
		auto transaction = node->store.tx_begin_read ();
		ASSERT_FALSE (store.account_get (transaction, nano::test_genesis_key.pub, account_info));
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, nano::test_genesis_key.pub, confirmation_height));
		ASSERT_EQ (4, confirmation_height);
		ASSERT_EQ (4, account_info.block_count);
		ASSERT_FALSE (store.account_get (transaction, key1.pub, account_info));
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, key1.pub, confirmation_height));
		ASSERT_EQ (2, confirmation_height);
		ASSERT_EQ (3, account_info.block_count);
		ASSERT_FALSE (store.account_get (transaction, key2.pub, account_info));
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, key2.pub, confirmation_height));
		ASSERT_EQ (3, confirmation_height);
		ASSERT_EQ (4, account_info.block_count);
		ASSERT_FALSE (store.account_get (transaction, key3.pub, account_info));
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, key3.pub, confirmation_height));
		ASSERT_EQ (2, confirmation_height);
		ASSERT_EQ (2, account_info.block_count);

		// The accounts for key1 and key2 have 1 more block in the chain than is confirmed.
		// So this can be rolled back, but the one before that cannot. Check that this is the case
		{
			auto transaction = node->store.tx_begin_write ();
			ASSERT_FALSE (node->ledger.rollback (transaction, node->latest (key2.pub)));
			ASSERT_FALSE (node->ledger.rollback (transaction, node->latest (key1.pub)));
		}
		{
			// These rollbacks should fail
			auto transaction = node->store.tx_begin_write ();
			ASSERT_TRUE (node->ledger.rollback (transaction, node->latest (key1.pub)));
			ASSERT_TRUE (node->ledger.rollback (transaction, node->latest (key2.pub)));

			// Confirm the other latest can't be rolled back either
			ASSERT_TRUE (node->ledger.rollback (transaction, node->latest (key3.pub)));
			ASSERT_TRUE (node->ledger.rollback (transaction, node->latest (nano::test_genesis_key.pub)));

			// Attempt some others which have been cemented
			ASSERT_TRUE (node->ledger.rollback (transaction, open1.hash ()));
			ASSERT_TRUE (node->ledger.rollback (transaction, send2.hash ()));
		}
		ASSERT_EQ (10, node->stats.count (nano::stat::type::confirmation_height, nano::stat::detail::blocks_confirmed, nano::stat::dir::in));
		ASSERT_EQ (10, node->stats.count (nano::stat::type::http_callback, nano::stat::detail::http_callback, nano::stat::dir::out));
	}
}

TEST (confirmation_height, gap_bootstrap)
{
	nano::system system (24000, 1);
	auto & node1 (*system.nodes[0]);
	nano::genesis genesis;
	nano::keypair destination;
	auto send1 (std::make_shared<nano::state_block> (nano::genesis_account, genesis.hash (), nano::genesis_account, nano::genesis_amount - nano::Gxrb_ratio, destination.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, 0));
	node1.work_generate_blocking (*send1);
	auto send2 (std::make_shared<nano::state_block> (nano::genesis_account, send1->hash (), nano::genesis_account, nano::genesis_amount - 2 * nano::Gxrb_ratio, destination.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, 0));
	node1.work_generate_blocking (*send2);
	auto send3 (std::make_shared<nano::state_block> (nano::genesis_account, send2->hash (), nano::genesis_account, nano::genesis_amount - 3 * nano::Gxrb_ratio, destination.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, 0));
	node1.work_generate_blocking (*send3);
	auto open1 (std::make_shared<nano::open_block> (send1->hash (), destination.pub, destination.pub, destination.prv, destination.pub, 0));
	node1.work_generate_blocking (*open1);

	// Receive
	auto receive1 (std::make_shared<nano::receive_block> (open1->hash (), send2->hash (), destination.prv, destination.pub, 0));
	node1.work_generate_blocking (*receive1);
	auto receive2 (std::make_shared<nano::receive_block> (receive1->hash (), send3->hash (), destination.prv, destination.pub, 0));
	node1.work_generate_blocking (*receive2);

	node1.block_processor.add (send1);
	node1.block_processor.add (send2);
	node1.block_processor.add (send3);
	node1.block_processor.add (receive1);
	node1.block_processor.flush ();

	add_callback_stats (node1);

	// Receive 2 comes in on the live network, however the chain has not been finished so it gets added to unchecked
	node1.process_active (receive2);
	node1.block_processor.flush ();

	// Confirmation heights should not be updated
	{
		auto transaction (node1.store.tx_begin_read ());
		auto unchecked_count (node1.store.unchecked_count (transaction));
		ASSERT_EQ (unchecked_count, 2);

		uint64_t confirmation_height;
		ASSERT_FALSE (node1.store.confirmation_height_get (transaction, nano::test_genesis_key.pub, confirmation_height));
		ASSERT_EQ (1, confirmation_height);
	}

	// Now complete the chain where the block comes in on the bootstrap network.
	node1.block_processor.add (open1);
	node1.block_processor.flush ();

	// Confirmation height should be unchanged and unchecked should now be 0
	{
		auto transaction (node1.store.tx_begin_read ());
		auto unchecked_count (node1.store.unchecked_count (transaction));
		ASSERT_EQ (unchecked_count, 0);

		uint64_t confirmation_height;
		ASSERT_FALSE (node1.store.confirmation_height_get (transaction, nano::test_genesis_key.pub, confirmation_height));
		ASSERT_EQ (1, confirmation_height);
		ASSERT_FALSE (node1.store.confirmation_height_get (transaction, destination.pub, confirmation_height));
		ASSERT_EQ (0, confirmation_height);
	}
	ASSERT_EQ (0, node1.stats.count (nano::stat::type::confirmation_height, nano::stat::detail::blocks_confirmed, nano::stat::dir::in));
	ASSERT_EQ (0, node1.stats.count (nano::stat::type::http_callback, nano::stat::detail::http_callback, nano::stat::dir::out));
}

TEST (confirmation_height, gap_live)
{
	nano::system system;
	nano::node_config node_config (24001, system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	system.add_node (node_config);
	node_config.peering_port = 24002;
	system.add_node (node_config);
	nano::keypair destination;
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	system.wallet (1)->insert_adhoc (destination.prv);

	nano::genesis genesis;
	auto send1 (std::make_shared<nano::state_block> (nano::genesis_account, genesis.hash (), nano::genesis_account, nano::genesis_amount - nano::Gxrb_ratio, destination.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, 0));
	system.nodes[0]->work_generate_blocking (*send1);
	auto send2 (std::make_shared<nano::state_block> (nano::genesis_account, send1->hash (), nano::genesis_account, nano::genesis_amount - 2 * nano::Gxrb_ratio, destination.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, 0));
	system.nodes[0]->work_generate_blocking (*send2);
	auto send3 (std::make_shared<nano::state_block> (nano::genesis_account, send2->hash (), nano::genesis_account, nano::genesis_amount - 3 * nano::Gxrb_ratio, destination.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, 0));
	system.nodes[0]->work_generate_blocking (*send3);

	auto open1 (std::make_shared<nano::open_block> (send1->hash (), destination.pub, destination.pub, destination.prv, destination.pub, 0));
	system.nodes[0]->work_generate_blocking (*open1);
	auto receive1 (std::make_shared<nano::receive_block> (open1->hash (), send2->hash (), destination.prv, destination.pub, 0));
	system.nodes[0]->work_generate_blocking (*receive1);
	auto receive2 (std::make_shared<nano::receive_block> (receive1->hash (), send3->hash (), destination.prv, destination.pub, 0));
	system.nodes[0]->work_generate_blocking (*receive2);

	for (auto & node : system.nodes)
	{
		node->block_processor.add (send1);
		node->block_processor.add (send2);
		node->block_processor.add (send3);
		node->block_processor.add (receive1);
		node->block_processor.flush ();

		add_callback_stats (*node);

		// Receive 2 comes in on the live network, however the chain has not been finished so it gets added to unchecked
		node->process_active (receive2);
		node->block_processor.flush ();

		// Confirmation heights should not be updated
		{
			auto transaction = node->store.tx_begin_read ();
			uint64_t confirmation_height;
			ASSERT_FALSE (node->store.confirmation_height_get (transaction, nano::test_genesis_key.pub, confirmation_height));
			ASSERT_EQ (1, confirmation_height);
		}

		// Now complete the chain where the block comes in on the live network
		node->process_active (open1);
		node->block_processor.flush ();

		system.deadline_set (10s);
		while (true)
		{
			auto transaction = node->store.tx_begin_read ();
			if (node->ledger.block_confirmed (transaction, receive2->hash ()))
			{
				break;
			}

			ASSERT_NO_ERROR (system.poll ());
		}

		// This should confirm the open block and the source of the receive blocks
		auto transaction (node->store.tx_begin_read ());
		auto unchecked_count (node->store.unchecked_count (transaction));
		ASSERT_EQ (unchecked_count, 0);

		uint64_t confirmation_height;
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, nano::test_genesis_key.pub, confirmation_height));
		ASSERT_EQ (4, confirmation_height);
		ASSERT_FALSE (node->store.confirmation_height_get (transaction, destination.pub, confirmation_height));
		ASSERT_EQ (3, confirmation_height);

		ASSERT_EQ (6, node->stats.count (nano::stat::type::confirmation_height, nano::stat::detail::blocks_confirmed, nano::stat::dir::in));
		ASSERT_EQ (6, node->stats.count (nano::stat::type::http_callback, nano::stat::detail::http_callback, nano::stat::dir::out));
	}
}

TEST (confirmation_height, send_receive_between_2_accounts)
{
	nano::system system;
	nano::node_config node_config (24000, system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto node = system.add_node (node_config);
	nano::keypair key1;
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	nano::block_hash latest (node->latest (nano::test_genesis_key.pub));
	system.wallet (0)->insert_adhoc (key1.prv);

	nano::send_block send1 (latest, key1.pub, node->config.online_weight_minimum.number () + 2, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (latest));
	nano::open_block open1 (send1.hash (), nano::genesis_account, key1.pub, key1.prv, key1.pub, system.work.generate (key1.pub));

	nano::send_block send2 (open1.hash (), nano::genesis_account, 1000, key1.prv, key1.pub, system.work.generate (open1.hash ()));
	nano::send_block send3 (send2.hash (), nano::genesis_account, 900, key1.prv, key1.pub, system.work.generate (send2.hash ()));
	nano::send_block send4 (send3.hash (), nano::genesis_account, 500, key1.prv, key1.pub, system.work.generate (send3.hash ()));

	nano::receive_block receive1 (send1.hash (), send2.hash (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (send1.hash ()));
	nano::receive_block receive2 (receive1.hash (), send3.hash (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (receive1.hash ()));
	nano::receive_block receive3 (receive2.hash (), send4.hash (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (receive2.hash ()));

	nano::send_block send5 (receive3.hash (), key1.pub, node->config.online_weight_minimum.number () + 1, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (receive3.hash ()));
	auto receive4 = std::make_shared<nano::receive_block> (send4.hash (), send5.hash (), key1.prv, key1.pub, system.work.generate (send4.hash ()));
	// Unpocketed send
	nano::keypair key2;
	nano::send_block send6 (send5.hash (), key2.pub, node->config.online_weight_minimum.number (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (send5.hash ()));
	{
		auto transaction = node->store.tx_begin_write ();
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send1).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, open1).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send2).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, receive1).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send3).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send4).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, receive2).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, receive3).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send5).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send6).code);
	}

	add_callback_stats (*node);

	node->process_active (receive4);
	node->block_processor.flush ();

	system.deadline_set (10s);
	while (true)
	{
		auto transaction = node->store.tx_begin_read ();
		if (node->ledger.block_confirmed (transaction, receive4->hash ()))
		{
			break;
		}

		ASSERT_NO_ERROR (system.poll ());
	}

	auto transaction (node->store.tx_begin_read ());

	nano::account_info account_info;
	uint64_t confirmation_height;
	ASSERT_FALSE (node->store.account_get (transaction, nano::test_genesis_key.pub, account_info));
	ASSERT_FALSE (node->store.confirmation_height_get (transaction, nano::test_genesis_key.pub, confirmation_height));
	ASSERT_EQ (6, confirmation_height);
	ASSERT_EQ (7, account_info.block_count);

	ASSERT_FALSE (node->store.account_get (transaction, key1.pub, account_info));
	ASSERT_FALSE (node->store.confirmation_height_get (transaction, key1.pub, confirmation_height));
	ASSERT_EQ (5, confirmation_height);
	ASSERT_EQ (5, account_info.block_count);

	ASSERT_EQ (10, node->stats.count (nano::stat::type::confirmation_height, nano::stat::detail::blocks_confirmed, nano::stat::dir::in));
	ASSERT_EQ (10, node->stats.count (nano::stat::type::http_callback, nano::stat::detail::http_callback, nano::stat::dir::out));
}

TEST (confirmation_height, send_receive_self)
{
	nano::system system;
	nano::node_config node_config (24000, system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto node = system.add_node (node_config);
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	nano::block_hash latest (node->latest (nano::test_genesis_key.pub));

	nano::send_block send1 (latest, nano::test_genesis_key.pub, nano::genesis_amount - 2, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (latest));
	nano::receive_block receive1 (send1.hash (), send1.hash (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (send1.hash ()));
	nano::send_block send2 (receive1.hash (), nano::test_genesis_key.pub, nano::genesis_amount - 2, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (receive1.hash ()));
	nano::send_block send3 (send2.hash (), nano::test_genesis_key.pub, nano::genesis_amount - 3, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (send2.hash ()));

	nano::receive_block receive2 (send3.hash (), send2.hash (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (send3.hash ()));
	auto receive3 = std::make_shared<nano::receive_block> (receive2.hash (), send3.hash (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (receive2.hash ()));

	// Send to another account to prevent automatic receiving on the genesis account
	nano::keypair key1;
	nano::send_block send4 (receive3->hash (), key1.pub, node->config.online_weight_minimum.number (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (receive3->hash ()));
	{
		auto transaction = node->store.tx_begin_write ();
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send1).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, receive1).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send2).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send3).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, receive2).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, *receive3).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send4).code);
	}

	add_callback_stats (*node);

	node->block_confirm (receive3);

	system.deadline_set (10s);
	while (true)
	{
		auto transaction = node->store.tx_begin_read ();
		if (node->ledger.block_confirmed (transaction, receive3->hash ()))
		{
			break;
		}

		ASSERT_NO_ERROR (system.poll ());
	}

	auto transaction (node->store.tx_begin_read ());
	nano::account_info account_info;
	ASSERT_FALSE (node->store.account_get (transaction, nano::test_genesis_key.pub, account_info));
	uint64_t confirmation_height;
	ASSERT_FALSE (node->store.confirmation_height_get (transaction, nano::test_genesis_key.pub, confirmation_height));
	ASSERT_EQ (7, confirmation_height);
	ASSERT_EQ (8, account_info.block_count);
	ASSERT_EQ (6, node->stats.count (nano::stat::type::confirmation_height, nano::stat::detail::blocks_confirmed, nano::stat::dir::in));
	ASSERT_EQ (6, node->stats.count (nano::stat::type::http_callback, nano::stat::detail::http_callback, nano::stat::dir::out));
}

TEST (confirmation_height, all_block_types)
{
	nano::system system;
	nano::node_config node_config (24000, system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto node = system.add_node (node_config);
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	nano::block_hash latest (node->latest (nano::test_genesis_key.pub));
	nano::keypair key1;
	nano::keypair key2;
	auto & store = node->store;
	nano::send_block send (latest, key1.pub, nano::genesis_amount - nano::Gxrb_ratio, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (latest));
	nano::send_block send1 (send.hash (), key2.pub, nano::genesis_amount - nano::Gxrb_ratio * 2, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (send.hash ()));

	nano::open_block open (send.hash (), nano::test_genesis_key.pub, key1.pub, key1.prv, key1.pub, system.work.generate (key1.pub));
	nano::state_block state_open (key2.pub, 0, 0, nano::Gxrb_ratio, send1.hash (), key2.prv, key2.pub, system.work.generate (key2.pub));

	nano::send_block send2 (open.hash (), key2.pub, 0, key1.prv, key1.pub, system.work.generate (open.hash ()));
	nano::state_block state_receive (key2.pub, state_open.hash (), 0, nano::Gxrb_ratio * 2, send2.hash (), key2.prv, key2.pub, system.work.generate (state_open.hash ()));

	nano::state_block state_send (key2.pub, state_receive.hash (), 0, nano::Gxrb_ratio, key1.pub, key2.prv, key2.pub, system.work.generate (state_receive.hash ()));
	nano::receive_block receive (send2.hash (), state_send.hash (), key1.prv, key1.pub, system.work.generate (send2.hash ()));

	nano::change_block change (receive.hash (), key2.pub, key1.prv, key1.pub, system.work.generate (receive.hash ()));

	nano::state_block state_change (key2.pub, state_send.hash (), nano::test_genesis_key.pub, nano::Gxrb_ratio, 0, key2.prv, key2.pub, system.work.generate (state_send.hash ()));

	nano::keypair epoch_key;
	node->ledger.epoch_signer = epoch_key.pub;

	nano::state_block epoch (key2.pub, state_change.hash (), nano::test_genesis_key.pub, nano::Gxrb_ratio, node->ledger.epoch_link, epoch_key.prv, epoch_key.pub, system.work.generate (state_change.hash ()));

	nano::state_block epoch1 (key1.pub, change.hash (), key2.pub, nano::Gxrb_ratio, node->ledger.epoch_link, epoch_key.prv, epoch_key.pub, system.work.generate (change.hash ()));
	nano::state_block state_send1 (key1.pub, epoch1.hash (), 0, nano::Gxrb_ratio - 1, key2.pub, key1.prv, key1.pub, system.work.generate (epoch1.hash ()));
	nano::state_block state_receive2 (key2.pub, epoch.hash (), 0, nano::Gxrb_ratio + 1, state_send1.hash (), key2.prv, key2.pub, system.work.generate (epoch.hash ()));

	auto state_send2 = std::make_shared<nano::state_block> (key2.pub, state_receive2.hash (), 0, nano::Gxrb_ratio, key1.pub, key2.prv, key2.pub, system.work.generate (state_receive2.hash ()));
	nano::state_block state_send3 (key2.pub, state_send2->hash (), 0, nano::Gxrb_ratio - 1, key1.pub, key2.prv, key2.pub, system.work.generate (state_send2->hash ()));

	nano::state_block state_send4 (key1.pub, state_send1.hash (), 0, nano::Gxrb_ratio - 2, nano::test_genesis_key.pub, key1.prv, key1.pub, system.work.generate (state_send1.hash ()));
	nano::state_block state_receive3 (nano::genesis_account, send1.hash (), nano::genesis_account, nano::genesis_amount - nano::Gxrb_ratio * 2 + 1, state_send4.hash (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (send1.hash ()));

	{
		auto transaction (store.tx_begin_write ());
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send1).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, open).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, state_open).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send2).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, state_receive).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, state_send).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, receive).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, change).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, state_change).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, epoch).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, epoch1).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, state_send1).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, state_receive2).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, *state_send2).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, state_send3).code);

		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, state_send4).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, state_receive3).code);
	}

	add_callback_stats (*node);
	node->block_confirm (state_send2);

	system.deadline_set (10s);
	while (true)
	{
		auto transaction = node->store.tx_begin_read ();
		if (node->ledger.block_confirmed (transaction, state_send2->hash ()))
		{
			break;
		}

		ASSERT_NO_ERROR (system.poll ());
	}

	auto transaction (node->store.tx_begin_read ());
	nano::account_info account_info;
	uint64_t confirmation_height;
	ASSERT_FALSE (node->store.account_get (transaction, nano::test_genesis_key.pub, account_info));
	ASSERT_FALSE (node->store.confirmation_height_get (transaction, nano::test_genesis_key.pub, confirmation_height));
	ASSERT_EQ (3, confirmation_height);
	ASSERT_LE (4, account_info.block_count);

	ASSERT_FALSE (node->store.account_get (transaction, key1.pub, account_info));
	ASSERT_FALSE (node->store.confirmation_height_get (transaction, key1.pub, confirmation_height));
	ASSERT_EQ (6, confirmation_height);
	ASSERT_LE (7, account_info.block_count);

	ASSERT_FALSE (node->store.account_get (transaction, key2.pub, account_info));
	ASSERT_FALSE (node->store.confirmation_height_get (transaction, key2.pub, confirmation_height));
	ASSERT_EQ (7, confirmation_height);
	ASSERT_LE (8, account_info.block_count);

	ASSERT_EQ (15, node->stats.count (nano::stat::type::confirmation_height, nano::stat::detail::blocks_confirmed, nano::stat::dir::in));
	ASSERT_EQ (15, node->stats.count (nano::stat::type::http_callback, nano::stat::detail::http_callback, nano::stat::dir::out));
}

/* Bulk of the this test was taken from the node.fork_flip test */
TEST (confirmation_height, conflict_rollback_cemented)
{
	boost::iostreams::stream_buffer<nano::stringstream_mt_sink> sb;
	sb.open (nano::stringstream_mt_sink{});
	nano::boost_log_cerr_redirect redirect_cerr (&sb);
	nano::system system (24000, 2);
	auto & node1 (*system.nodes[0]);
	auto & node2 (*system.nodes[1]);
	ASSERT_EQ (1, node1.network.size ());
	nano::keypair key1;
	nano::genesis genesis;
	auto send1 (std::make_shared<nano::send_block> (genesis.hash (), key1.pub, nano::genesis_amount - 100, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (genesis.hash ())));
	nano::publish publish1 (send1);
	nano::keypair key2;
	auto send2 (std::make_shared<nano::send_block> (genesis.hash (), key2.pub, nano::genesis_amount - 100, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (genesis.hash ())));
	nano::publish publish2 (send2);
	auto channel1 (node1.network.udp_channels.create (node1.network.endpoint ()));
	node1.network.process_message (publish1, channel1);
	node1.block_processor.flush ();
	auto channel2 (node2.network.udp_channels.create (node1.network.endpoint ()));
	node2.network.process_message (publish2, channel2);
	node2.block_processor.flush ();
	ASSERT_EQ (1, node1.active.size ());
	ASSERT_EQ (1, node2.active.size ());
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	node1.network.process_message (publish2, channel1);
	node1.block_processor.flush ();
	node2.network.process_message (publish1, channel2);
	node2.block_processor.flush ();
	std::unique_lock<std::mutex> lock (node2.active.mutex);
	auto conflict (node2.active.roots.find (nano::qualified_root (genesis.hash (), genesis.hash ())));
	ASSERT_NE (node2.active.roots.end (), conflict);
	auto votes1 (conflict->election);
	ASSERT_NE (nullptr, votes1);
	ASSERT_EQ (1, votes1->last_votes.size ());
	lock.unlock ();
	// Force blocks to be cemented on both nodes
	{
		auto transaction (system.nodes[0]->store.tx_begin_write ());
		ASSERT_TRUE (node1.store.block_exists (transaction, publish1.block->hash ()));
		node1.store.confirmation_height_put (transaction, nano::genesis_account, 2);
	}
	{
		auto transaction (system.nodes[1]->store.tx_begin_write ());
		ASSERT_TRUE (node2.store.block_exists (transaction, publish2.block->hash ()));
		node2.store.confirmation_height_put (transaction, nano::genesis_account, 2);
	}

	auto rollback_log_entry = boost::str (boost::format ("Failed to roll back %1%") % send2->hash ().to_string ());
	system.deadline_set (20s);
	auto done (false);
	while (!done)
	{
		ASSERT_NO_ERROR (system.poll ());
		done = (sb.component ()->str ().find (rollback_log_entry) != std::string::npos);
	}
	auto transaction1 (system.nodes[0]->store.tx_begin_read ());
	auto transaction2 (system.nodes[1]->store.tx_begin_read ());
	lock.lock ();
	auto winner (*votes1->tally (transaction2).begin ());
	ASSERT_EQ (*publish1.block, *winner.second);
	ASSERT_EQ (nano::genesis_amount - 100, winner.first);
	ASSERT_TRUE (node1.store.block_exists (transaction1, publish1.block->hash ()));
	ASSERT_TRUE (node2.store.block_exists (transaction2, publish2.block->hash ()));
	ASSERT_FALSE (node2.store.block_exists (transaction2, publish1.block->hash ()));
}

TEST (confirmation_height, observers)
{
	auto amount (std::numeric_limits<nano::uint128_t>::max ());
	nano::system system (24000, 1);
	auto node1 (system.nodes[0]);
	nano::keypair key1;
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	nano::block_hash latest1 (node1->latest (nano::test_genesis_key.pub));
	auto send1 (std::make_shared<nano::send_block> (latest1, key1.pub, amount - node1->config.receive_minimum.number (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (latest1)));

	add_callback_stats (*node1);

	node1->process_active (send1);
	node1->block_processor.flush ();
	bool confirmed (false);
	system.deadline_set (10s);
	while (!confirmed)
	{
		auto transaction = node1->store.tx_begin_read ();
		confirmed = node1->ledger.block_confirmed (transaction, send1->hash ());
		ASSERT_NO_ERROR (system.poll ());
	}
	ASSERT_EQ (1, node1->stats.count (nano::stat::type::confirmation_height, nano::stat::detail::blocks_confirmed, nano::stat::dir::in));
	ASSERT_EQ (1, node1->stats.count (nano::stat::type::http_callback, nano::stat::detail::http_callback, nano::stat::dir::out));
}

// This tests when a read has been done and the block no longer exists by the time a write is done
TEST (confirmation_height, modified_chain)
{
	nano::system system;
	nano::node_config node_config (24000, system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto node = system.add_node (node_config);

	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	nano::block_hash latest (node->latest (nano::test_genesis_key.pub));

	nano::keypair key1;
	auto & store = node->store;
	auto send = std::make_shared<nano::send_block> (latest, key1.pub, nano::genesis_amount - nano::Gxrb_ratio, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (latest));

	{
		auto transaction = node->store.tx_begin_write ();
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, *send).code);
	}

	node->confirmation_height_processor.add (send->hash ());

	{
		// The write guard prevents the confirmation height processor doing any writes
		auto write_guard = node->write_database_queue.wait (nano::writer::process_batch);
		while (!node->write_database_queue.contains (nano::writer::confirmation_height))
			;

		store.block_del (store.tx_begin_write (), send->hash ());
	}

	while (node->write_database_queue.contains (nano::writer::confirmation_height))
		;

	ASSERT_EQ (1, node->stats.count (nano::stat::type::confirmation_height, nano::stat::detail::invalid_block, nano::stat::dir::in));
}

namespace nano
{
TEST (confirmation_height, pending_observer_callbacks)
{
	nano::system system;
	nano::node_config node_config (24000, system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto node = system.add_node (node_config);

	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	nano::block_hash latest (node->latest (nano::test_genesis_key.pub));

	nano::keypair key1;
	nano::send_block send (latest, key1.pub, nano::genesis_amount - nano::Gxrb_ratio, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (latest));
	auto send1 = std::make_shared<nano::send_block> (send.hash (), key1.pub, nano::genesis_amount - nano::Gxrb_ratio * 2, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (send.hash ()));

	{
		auto transaction = node->store.tx_begin_write ();
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, send).code);
		ASSERT_EQ (nano::process_result::progress, node->ledger.process (transaction, *send1).code);
	}

	add_callback_stats (*node);

	node->confirmation_height_processor.add (send1->hash ());

	while (true)
	{
		if (node->pending_confirmation_height.size () == 0)
		{
			break;
		}
	}
	// Can have timing issues.
	node->confirmation_height_processor.add (send.hash ());
	{
		std::unique_lock<std::mutex> lk (node->pending_confirmation_height.mutex);
		while (!node->pending_confirmation_height.current_hash.is_zero ())
		{
			lk.unlock ();
			std::this_thread::yield ();
			lk.lock ();
		}
	}

	// Confirm the callback is not called under this circumstance
	ASSERT_EQ (2, node->stats.count (nano::stat::type::confirmation_height, nano::stat::detail::blocks_confirmed, nano::stat::dir::in));
	ASSERT_EQ (0, node->stats.count (nano::stat::type::http_callback, nano::stat::detail::http_callback, nano::stat::dir::out));
}
}

TEST (bootstrap, tcp_listener_timeout_empty)
{
	nano::system system (24000, 1);
	auto node0 (system.nodes[0]);
	auto socket (std::make_shared<nano::socket> (node0));
	std::atomic<bool> connected (false);
	socket->async_connect (node0->bootstrap.endpoint (), [&connected](boost::system::error_code const & ec) {
		ASSERT_FALSE (ec);
		connected = true;
	});
	system.deadline_set (std::chrono::seconds (5));
	while (!connected)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	bool disconnected (false);
	system.deadline_set (std::chrono::seconds (6));
	while (!disconnected)
	{
		{
			std::lock_guard<std::mutex> guard (node0->bootstrap.mutex);
			disconnected = node0->bootstrap.connections.empty ();
		}
		ASSERT_NO_ERROR (system.poll ());
	}
}

TEST (bootstrap, tcp_listener_timeout_node_id_handshake)
{
	nano::system system (24000, 1);
	auto node0 (system.nodes[0]);
	auto socket (std::make_shared<nano::socket> (node0));
	auto cookie (node0->network.syn_cookies.assign (nano::transport::map_tcp_to_endpoint (node0->bootstrap.endpoint ())));
	nano::node_id_handshake node_id_handshake (cookie, boost::none);
	auto input (node_id_handshake.to_bytes ());
	socket->async_connect (node0->bootstrap.endpoint (), [&input, socket](boost::system::error_code const & ec) {
		ASSERT_FALSE (ec);
		socket->async_write (input, [&input](boost::system::error_code const & ec, size_t size_a) {
			ASSERT_FALSE (ec);
			ASSERT_EQ (input->size (), size_a);
		});
	});
	system.deadline_set (std::chrono::seconds (5));
	while (node0->stats.count (nano::stat::type::message, nano::stat::detail::node_id_handshake) == 0)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	{
		std::lock_guard<std::mutex> guard (node0->bootstrap.mutex);
		ASSERT_EQ (node0->bootstrap.connections.size (), 1);
	}
	bool disconnected (false);
	system.deadline_set (std::chrono::seconds (20));
	while (!disconnected)
	{
		{
			std::lock_guard<std::mutex> guard (node0->bootstrap.mutex);
			disconnected = node0->bootstrap.connections.empty ();
		}
		ASSERT_NO_ERROR (system.poll ());
	}
}

TEST (network, replace_port)
{
	nano::system system (24000, 1);
	ASSERT_EQ (0, system.nodes[0]->network.size ());
	auto node1 (std::make_shared<nano::node> (system.io_ctx, 24001, nano::unique_path (), system.alarm, system.logging, system.work));
	node1->start ();
	system.nodes.push_back (node1);
	{
		auto channel (system.nodes[0]->network.udp_channels.insert (nano::endpoint (node1->network.endpoint ().address (), 23000), node1->network_params.protocol.protocol_version));
		if (channel)
		{
			channel->set_node_id (node1->node_id.pub);
		}
	}
	auto peers_list (system.nodes[0]->network.list (std::numeric_limits<size_t>::max ()));
	ASSERT_EQ (peers_list[0]->get_node_id (), node1->node_id.pub);
	auto channel (std::make_shared<nano::transport::channel_udp> (system.nodes[0]->network.udp_channels, node1->network.endpoint (), node1->network_params.protocol.protocol_version));
	system.nodes[0]->network.send_keepalive (channel);
	system.deadline_set (5s);
	while (!system.nodes[0]->network.udp_channels.channel (node1->network.endpoint ()))
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	system.deadline_set (5s);
	while (system.nodes[0]->network.udp_channels.size () > 1)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	ASSERT_EQ (system.nodes[0]->network.udp_channels.size (), 1);
	auto list1 (system.nodes[0]->network.list (1));
	ASSERT_EQ (node1->network.endpoint (), list1[0]->get_endpoint ());
	auto list2 (node1->network.list (1));
	ASSERT_EQ (system.nodes[0]->network.endpoint (), list2[0]->get_endpoint ());
	// Remove correct peer (same node ID)
	system.nodes[0]->network.udp_channels.clean_node_id (nano::endpoint (node1->network.endpoint ().address (), 23000), node1->node_id.pub);
	ASSERT_EQ (system.nodes[0]->network.udp_channels.size (), 0);
	node1->stop ();
}

TEST (bandwidth_limiter, validate)
{
	size_t const full_confirm_ack (488 + 8);
	{
		nano::bandwidth_limiter limiter_0 (0);
		nano::bandwidth_limiter limiter_1 (1024);
		nano::bandwidth_limiter limiter_256 (1024 * 256);
		nano::bandwidth_limiter limiter_1024 (1024 * 1024);
		nano::bandwidth_limiter limiter_1536 (1024 * 1536);

		auto now (std::chrono::steady_clock::now ());

		while (now + 1s >= std::chrono::steady_clock::now ())
		{
			ASSERT_FALSE (limiter_0.should_drop (full_confirm_ack)); // will never drop
			ASSERT_TRUE (limiter_1.should_drop (full_confirm_ack)); // always drop as message > limit / rate_buffer.size ()
			limiter_256.should_drop (full_confirm_ack);
			limiter_1024.should_drop (full_confirm_ack);
			limiter_1536.should_drop (full_confirm_ack);
			std::this_thread::sleep_for (10ms);
		}
		ASSERT_FALSE (limiter_0.should_drop (full_confirm_ack)); // will never drop
		ASSERT_TRUE (limiter_1.should_drop (full_confirm_ack)); // always drop as message > limit / rate_buffer.size ()
		ASSERT_FALSE (limiter_256.should_drop (full_confirm_ack)); // as a second has passed counter is started and nothing is dropped
		ASSERT_FALSE (limiter_1024.should_drop (full_confirm_ack)); // as a second has passed counter is started and nothing is dropped
		ASSERT_FALSE (limiter_1536.should_drop (full_confirm_ack)); // as a second has passed counter is started and nothing is dropped
	}

	{
		nano::bandwidth_limiter limiter_0 (0);
		nano::bandwidth_limiter limiter_1 (1024);
		nano::bandwidth_limiter limiter_256 (1024 * 256);
		nano::bandwidth_limiter limiter_1024 (1024 * 1024);
		nano::bandwidth_limiter limiter_1536 (1024 * 1536);

		auto now (std::chrono::steady_clock::now ());
		//trend rate for 5 sec
		while (now + 5s >= std::chrono::steady_clock::now ())
		{
			ASSERT_FALSE (limiter_0.should_drop (full_confirm_ack)); // will never drop
			ASSERT_TRUE (limiter_1.should_drop (full_confirm_ack)); // always drop as message > limit / rate_buffer.size ()
			limiter_256.should_drop (full_confirm_ack);
			limiter_1024.should_drop (full_confirm_ack);
			limiter_1536.should_drop (full_confirm_ack);
			std::this_thread::sleep_for (50ms);
		}
		ASSERT_EQ (limiter_0.get_rate (), 0); //should be 0 as rate is not gathered if not needed
		ASSERT_EQ (limiter_1.get_rate (), 0); //should be 0 since nothing is small enough to pass through is tracked
		ASSERT_EQ (limiter_256.get_rate (), full_confirm_ack); //should be 0 since nothing is small enough to pass through is tracked
		ASSERT_EQ (limiter_1024.get_rate (), full_confirm_ack); //should be 0 since nothing is small enough to pass through is tracked
		ASSERT_EQ (limiter_1536.get_rate (), full_confirm_ack); //should be 0 since nothing is small enough to pass through is tracked
	}
}
