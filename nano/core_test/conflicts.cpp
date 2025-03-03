#include <nano/core_test/testutil.hpp>
#include <nano/node/testing.hpp>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

TEST (conflicts, start_stop)
{
	nano::system system (24000, 1);
	auto & node1 (*system.nodes[0]);
	nano::genesis genesis;
	nano::keypair key1;
	auto send1 (std::make_shared<nano::send_block> (genesis.hash (), key1.pub, 0, nano::test_genesis_key.prv, nano::test_genesis_key.pub, 0));
	node1.work_generate_blocking (*send1);
	ASSERT_EQ (nano::process_result::progress, node1.process (*send1).code);
	ASSERT_EQ (0, node1.active.size ());
	node1.active.start (send1);
	ASSERT_EQ (1, node1.active.size ());
	{
		std::lock_guard<std::mutex> guard (node1.active.mutex);
		auto existing1 (node1.active.roots.find (send1->qualified_root ()));
		ASSERT_NE (node1.active.roots.end (), existing1);
		auto votes1 (existing1->election);
		ASSERT_NE (nullptr, votes1);
		ASSERT_EQ (1, votes1->last_votes.size ());
	}
}

TEST (conflicts, add_existing)
{
	nano::system system (24000, 1);
	auto & node1 (*system.nodes[0]);
	nano::genesis genesis;
	nano::keypair key1;
	auto send1 (std::make_shared<nano::send_block> (genesis.hash (), key1.pub, 0, nano::test_genesis_key.prv, nano::test_genesis_key.pub, 0));
	node1.work_generate_blocking (*send1);
	ASSERT_EQ (nano::process_result::progress, node1.process (*send1).code);
	node1.active.start (send1);
	nano::keypair key2;
	auto send2 (std::make_shared<nano::send_block> (genesis.hash (), key2.pub, 0, nano::test_genesis_key.prv, nano::test_genesis_key.pub, 0));
	node1.active.start (send2);
	ASSERT_EQ (1, node1.active.size ());
	auto vote1 (std::make_shared<nano::vote> (key2.pub, key2.prv, 0, send2));
	node1.active.vote (vote1);
	ASSERT_EQ (1, node1.active.size ());
	{
		std::lock_guard<std::mutex> guard (node1.active.mutex);
		auto votes1 (node1.active.roots.find (send2->qualified_root ())->election);
		ASSERT_NE (nullptr, votes1);
		ASSERT_EQ (2, votes1->last_votes.size ());
		ASSERT_NE (votes1->last_votes.end (), votes1->last_votes.find (key2.pub));
	}
}

TEST (conflicts, add_two)
{
	nano::system system (24000, 1);
	auto & node1 (*system.nodes[0]);
	nano::genesis genesis;
	nano::keypair key1;
	auto send1 (std::make_shared<nano::send_block> (genesis.hash (), key1.pub, 0, nano::test_genesis_key.prv, nano::test_genesis_key.pub, 0));
	node1.work_generate_blocking (*send1);
	ASSERT_EQ (nano::process_result::progress, node1.process (*send1).code);
	node1.active.start (send1);
	nano::keypair key2;
	auto send2 (std::make_shared<nano::send_block> (send1->hash (), key2.pub, 0, nano::test_genesis_key.prv, nano::test_genesis_key.pub, 0));
	node1.work_generate_blocking (*send2);
	ASSERT_EQ (nano::process_result::progress, node1.process (*send2).code);
	node1.active.start (send2);
	ASSERT_EQ (2, node1.active.size ());
}

TEST (vote_uniquer, null)
{
	nano::block_uniquer block_uniquer;
	nano::vote_uniquer uniquer (block_uniquer);
	ASSERT_EQ (nullptr, uniquer.unique (nullptr));
}

// Show that an identical vote can be uniqued
TEST (vote_uniquer, same_vote)
{
	nano::block_uniquer block_uniquer;
	nano::vote_uniquer uniquer (block_uniquer);
	nano::keypair key;
	auto vote1 (std::make_shared<nano::vote> (key.pub, key.prv, 0, std::make_shared<nano::state_block> (0, 0, 0, 0, 0, key.prv, key.pub, 0)));
	auto vote2 (std::make_shared<nano::vote> (*vote1));
	ASSERT_EQ (vote1, uniquer.unique (vote1));
	ASSERT_EQ (vote1, uniquer.unique (vote2));
}

// Show that a different vote for the same block will have the block uniqued
TEST (vote_uniquer, same_block)
{
	nano::block_uniquer block_uniquer;
	nano::vote_uniquer uniquer (block_uniquer);
	nano::keypair key1;
	nano::keypair key2;
	auto block1 (std::make_shared<nano::state_block> (0, 0, 0, 0, 0, key1.prv, key1.pub, 0));
	auto block2 (std::make_shared<nano::state_block> (*block1));
	auto vote1 (std::make_shared<nano::vote> (key1.pub, key1.prv, 0, block1));
	auto vote2 (std::make_shared<nano::vote> (key1.pub, key1.prv, 0, block2));
	ASSERT_EQ (vote1, uniquer.unique (vote1));
	ASSERT_EQ (vote2, uniquer.unique (vote2));
	ASSERT_NE (vote1, vote2);
	ASSERT_EQ (boost::get<std::shared_ptr<nano::block>> (vote1->blocks[0]), boost::get<std::shared_ptr<nano::block>> (vote2->blocks[0]));
}

TEST (vote_uniquer, vbh_one)
{
	nano::block_uniquer block_uniquer;
	nano::vote_uniquer uniquer (block_uniquer);
	nano::keypair key;
	auto block (std::make_shared<nano::state_block> (0, 0, 0, 0, 0, key.prv, key.pub, 0));
	std::vector<nano::block_hash> hashes;
	hashes.push_back (block->hash ());
	auto vote1 (std::make_shared<nano::vote> (key.pub, key.prv, 0, hashes));
	auto vote2 (std::make_shared<nano::vote> (*vote1));
	ASSERT_EQ (vote1, uniquer.unique (vote1));
	ASSERT_EQ (vote1, uniquer.unique (vote2));
}

TEST (vote_uniquer, vbh_two)
{
	nano::block_uniquer block_uniquer;
	nano::vote_uniquer uniquer (block_uniquer);
	nano::keypair key;
	auto block1 (std::make_shared<nano::state_block> (0, 0, 0, 0, 0, key.prv, key.pub, 0));
	std::vector<nano::block_hash> hashes1;
	hashes1.push_back (block1->hash ());
	auto block2 (std::make_shared<nano::state_block> (1, 0, 0, 0, 0, key.prv, key.pub, 0));
	std::vector<nano::block_hash> hashes2;
	hashes2.push_back (block2->hash ());
	auto vote1 (std::make_shared<nano::vote> (key.pub, key.prv, 0, hashes1));
	auto vote2 (std::make_shared<nano::vote> (key.pub, key.prv, 0, hashes2));
	ASSERT_EQ (vote1, uniquer.unique (vote1));
	ASSERT_EQ (vote2, uniquer.unique (vote2));
}

TEST (vote_uniquer, cleanup)
{
	nano::block_uniquer block_uniquer;
	nano::vote_uniquer uniquer (block_uniquer);
	nano::keypair key;
	auto vote1 (std::make_shared<nano::vote> (key.pub, key.prv, 0, std::make_shared<nano::state_block> (0, 0, 0, 0, 0, key.prv, key.pub, 0)));
	auto vote2 (std::make_shared<nano::vote> (key.pub, key.prv, 1, std::make_shared<nano::state_block> (0, 0, 0, 0, 0, key.prv, key.pub, 0)));
	auto vote3 (uniquer.unique (vote1));
	auto vote4 (uniquer.unique (vote2));
	vote2.reset ();
	vote4.reset ();
	ASSERT_EQ (2, uniquer.size ());
	auto iterations (0);
	while (uniquer.size () == 2)
	{
		auto vote5 (uniquer.unique (vote1));
		ASSERT_LT (iterations++, 200);
	}
}

TEST (conflicts, reprioritize)
{
	nano::system system (24000, 1);
	auto & node1 (*system.nodes[0]);
	nano::genesis genesis;
	nano::keypair key1;
	auto send1 (std::make_shared<nano::send_block> (genesis.hash (), key1.pub, 0, nano::test_genesis_key.prv, nano::test_genesis_key.pub, 0));
	node1.work_generate_blocking (*send1);
	uint64_t difficulty1;
	nano::work_validate (*send1, &difficulty1);
	nano::send_block send1_copy (*send1);
	node1.process_active (send1);
	node1.block_processor.flush ();
	{
		std::lock_guard<std::mutex> guard (node1.active.mutex);
		auto existing1 (node1.active.roots.find (send1->qualified_root ()));
		ASSERT_NE (node1.active.roots.end (), existing1);
		ASSERT_EQ (difficulty1, existing1->difficulty);
	}
	node1.work_generate_blocking (send1_copy, difficulty1);
	uint64_t difficulty2;
	nano::work_validate (send1_copy, &difficulty2);
	node1.process_active (std::make_shared<nano::send_block> (send1_copy));
	node1.block_processor.flush ();
	{
		std::lock_guard<std::mutex> guard (node1.active.mutex);
		auto existing2 (node1.active.roots.find (send1->qualified_root ()));
		ASSERT_NE (node1.active.roots.end (), existing2);
		ASSERT_EQ (difficulty2, existing2->difficulty);
	}
}

TEST (conflicts, dependency)
{
	nano::system system;
	nano::node_config node_config (24000, system.logging);
	node_config.frontiers_confirmation = nano::frontiers_confirmation_mode::disabled;
	auto node1 = system.add_node (node_config);
	nano::genesis genesis;
	nano::keypair key1;
	auto send1 (std::make_shared<nano::send_block> (genesis.hash (), key1.pub, nano::genesis_amount - nano::xrb_ratio, nano::test_genesis_key.prv, nano::test_genesis_key.pub, 0));
	node1->work_generate_blocking (*send1);
	auto state_open1 (std::make_shared<nano::state_block> (key1.pub, 0, nano::test_genesis_key.pub, nano::xrb_ratio, send1->hash (), key1.prv, key1.pub, 0));
	node1->work_generate_blocking (*state_open1);
	ASSERT_EQ (nano::process_result::progress, node1->process (*send1).code);
	ASSERT_EQ (nano::process_result::progress, node1->process (*state_open1).code);
	ASSERT_EQ (0, node1->active.size ());
	node1->active.start (send1);
	node1->active.start (state_open1);
	ASSERT_EQ (2, node1->active.size ());
	// Check dependency for send block
	{
		std::lock_guard<std::mutex> guard (node1->active.mutex);
		auto existing1 (node1->active.roots.find (send1->qualified_root ()));
		ASSERT_NE (node1->active.roots.end (), existing1);
		auto election1 (existing1->election);
		ASSERT_NE (nullptr, election1);
		ASSERT_EQ (1, election1->dependent_blocks.size ());
		ASSERT_NE (election1->dependent_blocks.end (), election1->dependent_blocks.find (state_open1->hash ()));
	}
}

TEST (conflicts, adjusted_difficulty)
{
	nano::system system (24000, 1);
	auto & node1 (*system.nodes[0]);
	nano::genesis genesis;
	nano::keypair key1;
	nano::keypair key2;
	nano::keypair key3;
	ASSERT_EQ (0, node1.active.size ());
	auto send1 (std::make_shared<nano::send_block> (genesis.hash (), key1.pub, nano::genesis_amount - 2 * nano::xrb_ratio, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (genesis.hash ())));
	node1.process_active (send1);
	auto send2 (std::make_shared<nano::send_block> (send1->hash (), nano::test_genesis_key.pub, nano::genesis_amount - 3 * nano::xrb_ratio, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (send1->hash ())));
	node1.process_active (send2);
	auto receive1 (std::make_shared<nano::receive_block> (send2->hash (), send2->hash (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (send2->hash ())));
	node1.process_active (receive1);
	auto open1 (std::make_shared<nano::open_block> (send1->hash (), key1.pub, key1.pub, key1.prv, key1.pub, system.work.generate (key1.pub)));
	node1.process_active (open1);
	auto send3 (std::make_shared<nano::state_block> (key1.pub, open1->hash (), key1.pub, nano::xrb_ratio, key2.pub, key1.prv, key1.pub, system.work.generate (open1->hash ())));
	node1.process_active (send3);
	auto send4 (std::make_shared<nano::state_block> (key1.pub, send3->hash (), key1.pub, 0, key3.pub, key1.prv, key1.pub, system.work.generate (send3->hash ())));
	node1.process_active (send4);
	ASSERT_EQ (node1.ledger.epoch_signer, nano::test_genesis_key.pub);
	auto open_epoch1 (std::make_shared<nano::state_block> (key2.pub, 0, 0, 0, node1.ledger.epoch_link, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (key2.pub)));
	node1.process_active (open_epoch1);
	auto receive2 (std::make_shared<nano::state_block> (key2.pub, open_epoch1->hash (), 0, nano::xrb_ratio, send3->hash (), key2.prv, key2.pub, system.work.generate (open_epoch1->hash ())));
	node1.process_active (receive2);
	auto open2 (std::make_shared<nano::state_block> (key3.pub, 0, key3.pub, nano::xrb_ratio, send4->hash (), key3.prv, key3.pub, system.work.generate (key3.pub)));
	node1.process_active (open2);
	auto change1 (std::make_shared<nano::state_block> (key3.pub, open2->hash (), nano::test_genesis_key.pub, nano::xrb_ratio, 0, key3.prv, key3.pub, system.work.generate (open2->hash ())));
	node1.process_active (change1);
	node1.block_processor.flush ();
	system.deadline_set (3s);
	while (node1.active.size () != 10)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	std::unordered_map<nano::block_hash, uint64_t> adjusted_difficulties;
	{
		std::lock_guard<std::mutex> guard (node1.active.mutex);
		ASSERT_EQ (node1.active.roots.get<1> ().begin ()->election->status.winner->hash (), send1->hash ());
		for (auto i (node1.active.roots.get<1> ().begin ()), n (node1.active.roots.get<1> ().end ()); i != n; ++i)
		{
			adjusted_difficulties.insert (std::make_pair (i->election->status.winner->hash (), i->adjusted_difficulty));
		}
	}
	// genesis
	ASSERT_GT (adjusted_difficulties.find (send1->hash ())->second, adjusted_difficulties.find (send2->hash ())->second);
	ASSERT_GT (adjusted_difficulties.find (send2->hash ())->second, adjusted_difficulties.find (receive1->hash ())->second);
	// key1
	ASSERT_GT (adjusted_difficulties.find (send1->hash ())->second, adjusted_difficulties.find (open1->hash ())->second);
	ASSERT_GT (adjusted_difficulties.find (open1->hash ())->second, adjusted_difficulties.find (send3->hash ())->second);
	ASSERT_GT (adjusted_difficulties.find (send3->hash ())->second, adjusted_difficulties.find (send4->hash ())->second);
	//key2
	ASSERT_GT (adjusted_difficulties.find (send3->hash ())->second, adjusted_difficulties.find (receive2->hash ())->second);
	ASSERT_GT (adjusted_difficulties.find (open_epoch1->hash ())->second, adjusted_difficulties.find (receive2->hash ())->second);
	// key3
	ASSERT_GT (adjusted_difficulties.find (send4->hash ())->second, adjusted_difficulties.find (open2->hash ())->second);
	ASSERT_GT (adjusted_difficulties.find (open2->hash ())->second, adjusted_difficulties.find (change1->hash ())->second);
	// Independent elections can have higher difficulty than adjusted tree
	nano::keypair key4;
	auto open_epoch2 (std::make_shared<nano::state_block> (key4.pub, 0, 0, 0, node1.ledger.epoch_link, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (key4.pub, adjusted_difficulties.find (send1->hash ())->second)));
	uint64_t difficulty;
	ASSERT_FALSE (nano::work_validate (*open_epoch2, &difficulty));
	ASSERT_GT (difficulty, adjusted_difficulties.find (send1->hash ())->second);
	node1.process_active (open_epoch2);
	node1.block_processor.flush ();
	system.deadline_set (3s);
	while (node1.active.size () != 11)
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	{
		std::lock_guard<std::mutex> guard (node1.active.mutex);
		ASSERT_EQ (node1.active.roots.get<1> ().begin ()->election->status.winner->hash (), open_epoch2->hash ());
	}
}
