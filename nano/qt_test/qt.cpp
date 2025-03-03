#include <nano/core_test/testutil.hpp>
#include <nano/node/testing.hpp>
#include <nano/qt/qt.hpp>

#include <gtest/gtest.h>

#include <boost/property_tree/json_parser.hpp>

#include <algorithm>
#include <nano/qt_test/QTest>
#include <thread>

using namespace std::chrono_literals;

extern QApplication * test_application;

TEST (wallet, construction)
{
	nano_qt::eventloop_processor processor;
	nano::system system (24000, 1);
	auto wallet_l (system.nodes[0]->wallets.create (nano::uint256_union ()));
	auto key (wallet_l->deterministic_insert ());
	auto wallet (std::make_shared<nano_qt::wallet> (*test_application, processor, *system.nodes[0], wallet_l, key));
	wallet->start ();
	std::string account (key.to_account ());
	ASSERT_EQ (account, wallet->self.account_text->text ().toStdString ());
	ASSERT_EQ (1, wallet->accounts.model->rowCount ());
	auto item1 (wallet->accounts.model->item (0, 1));
	ASSERT_EQ (key.to_account (), item1->text ().toStdString ());
}

TEST (wallet, status)
{
	nano_qt::eventloop_processor processor;
	nano::system system (24000, 1);
	auto wallet_l (system.nodes[0]->wallets.create (nano::uint256_union ()));
	nano::keypair key;
	wallet_l->insert_adhoc (key.prv);
	auto wallet (std::make_shared<nano_qt::wallet> (*test_application, processor, *system.nodes[0], wallet_l, key.pub));
	wallet->start ();
	auto wallet_has = [wallet](nano_qt::status_types status_ty) {
		return wallet->active_status.active.find (status_ty) != wallet->active_status.active.end ();
	};
	ASSERT_EQ ("Status: Disconnected, Blocks: 1", wallet->status->text ().toStdString ());
	system.nodes[0]->network.udp_channels.insert (nano::endpoint (boost::asio::ip::address_v6::loopback (), 10000), 0);
	// Because of the wallet "vulnerable" message, this won't be the message displayed.
	// However, it will still be part of the status set.
	ASSERT_FALSE (wallet_has (nano_qt::status_types::synchronizing));
	system.deadline_set (25s);
	while (!wallet_has (nano_qt::status_types::synchronizing))
	{
		test_application->processEvents ();
		ASSERT_NO_ERROR (system.poll ());
	}
	system.nodes[0]->network.cleanup (std::chrono::steady_clock::now () + std::chrono::seconds (5));
	while (wallet_has (nano_qt::status_types::synchronizing))
	{
		test_application->processEvents ();
	}
	ASSERT_TRUE (wallet_has (nano_qt::status_types::disconnected));
}

TEST (wallet, startup_balance)
{
	nano_qt::eventloop_processor processor;
	nano::system system (24000, 1);
	auto wallet_l (system.nodes[0]->wallets.create (nano::uint256_union ()));
	nano::keypair key;
	wallet_l->insert_adhoc (key.prv);
	auto wallet (std::make_shared<nano_qt::wallet> (*test_application, processor, *system.nodes[0], wallet_l, key.pub));
	wallet->needs_balance_refresh = true;
	wallet->start ();
	wallet->application.processEvents (QEventLoop::AllEvents);
	ASSERT_EQ ("Balance: 0 NANO", wallet->self.balance_label->text ().toStdString ());
}

TEST (wallet, select_account)
{
	nano_qt::eventloop_processor processor;
	nano::system system (24000, 1);
	auto wallet_l (system.nodes[0]->wallets.create (nano::uint256_union ()));
	nano::public_key key1 (wallet_l->deterministic_insert ());
	nano::public_key key2 (wallet_l->deterministic_insert ());
	auto wallet (std::make_shared<nano_qt::wallet> (*test_application, processor, *system.nodes[0], wallet_l, key1));
	wallet->start ();
	ASSERT_EQ (key1, wallet->account);
	QTest::mouseClick (wallet->show_advanced, Qt::LeftButton);
	QTest::mouseClick (wallet->accounts_button, Qt::LeftButton);
	wallet->accounts.view->selectionModel ()->setCurrentIndex (wallet->accounts.model->index (0, 0), QItemSelectionModel::SelectionFlag::Select);
	QTest::mouseClick (wallet->accounts.use_account, Qt::LeftButton);
	auto key3 (wallet->account);
	wallet->accounts.view->selectionModel ()->setCurrentIndex (wallet->accounts.model->index (1, 0), QItemSelectionModel::SelectionFlag::Select);
	QTest::mouseClick (wallet->accounts.use_account, Qt::LeftButton);
	auto key4 (wallet->account);
	ASSERT_NE (key3, key4);

	// The list is populated in sorted order as it's read from store in lexical order. This may
	// be different from the insertion order.
	if (key1 < key2)
	{
		ASSERT_EQ (key2, key4);
	}
	else
	{
		ASSERT_EQ (key1, key4);
	}
}

TEST (wallet, main)
{
	nano_qt::eventloop_processor processor;
	nano::system system (24000, 1);
	auto wallet_l (system.nodes[0]->wallets.create (nano::uint256_union ()));
	nano::keypair key;
	wallet_l->insert_adhoc (key.prv);
	auto wallet (std::make_shared<nano_qt::wallet> (*test_application, processor, *system.nodes[0], wallet_l, key.pub));
	wallet->start ();
	ASSERT_EQ (wallet->entry_window, wallet->main_stack->currentWidget ());
	QTest::mouseClick (wallet->send_blocks, Qt::LeftButton);
	ASSERT_EQ (wallet->send_blocks_window, wallet->main_stack->currentWidget ());
	QTest::mouseClick (wallet->send_blocks_back, Qt::LeftButton);
	QTest::mouseClick (wallet->settings_button, Qt::LeftButton);
	ASSERT_EQ (wallet->settings.window, wallet->main_stack->currentWidget ());
	QTest::mouseClick (wallet->settings.back, Qt::LeftButton);
	ASSERT_EQ (wallet->entry_window, wallet->main_stack->currentWidget ());
	QTest::mouseClick (wallet->show_advanced, Qt::LeftButton);
	ASSERT_EQ (wallet->advanced.window, wallet->main_stack->currentWidget ());
	QTest::mouseClick (wallet->advanced.show_ledger, Qt::LeftButton);
	ASSERT_EQ (wallet->advanced.ledger_window, wallet->main_stack->currentWidget ());
	QTest::mouseClick (wallet->advanced.ledger_back, Qt::LeftButton);
	ASSERT_EQ (wallet->advanced.window, wallet->main_stack->currentWidget ());
	QTest::mouseClick (wallet->advanced.show_peers, Qt::LeftButton);
	ASSERT_EQ (wallet->advanced.peers_window, wallet->main_stack->currentWidget ());
	QTest::mouseClick (wallet->advanced.peers_back, Qt::LeftButton);
	ASSERT_EQ (wallet->advanced.window, wallet->main_stack->currentWidget ());
	QTest::mouseClick (wallet->advanced.back, Qt::LeftButton);
	ASSERT_EQ (wallet->entry_window, wallet->main_stack->currentWidget ());
}

TEST (wallet, password_change)
{
	nano_qt::eventloop_processor processor;
	nano::system system (24000, 1);
	nano::account account;
	system.wallet (0)->insert_adhoc (nano::keypair ().prv);
	{
		auto transaction (system.nodes[0]->wallets.tx_begin_read ());
		account = system.account (transaction, 0);
	}
	auto wallet (std::make_shared<nano_qt::wallet> (*test_application, processor, *system.nodes[0], system.wallet (0), account));
	wallet->start ();
	QTest::mouseClick (wallet->settings_button, Qt::LeftButton);
	{
		auto transaction (system.nodes[0]->wallets.tx_begin_read ());
		nano::raw_key password1;
		nano::raw_key password2;
		system.wallet (0)->store.derive_key (password1, transaction, "1");
		system.wallet (0)->store.password.value (password2);
		ASSERT_NE (password1, password2);
	}
	QTest::keyClicks (wallet->settings.new_password, "1");
	QTest::keyClicks (wallet->settings.retype_password, "1");
	QTest::mouseClick (wallet->settings.change, Qt::LeftButton);
	{
		auto transaction (system.nodes[0]->wallets.tx_begin_read ());
		nano::raw_key password1;
		nano::raw_key password2;
		system.wallet (0)->store.derive_key (password1, transaction, "1");
		system.wallet (0)->store.password.value (password2);
		ASSERT_EQ (password1, password2);
	}
	ASSERT_EQ ("", wallet->settings.new_password->text ());
	ASSERT_EQ ("", wallet->settings.retype_password->text ());
}

TEST (client, password_nochange)
{
	nano_qt::eventloop_processor processor;
	nano::system system (24000, 1);
	nano::account account;
	system.wallet (0)->insert_adhoc (nano::keypair ().prv);
	{
		auto transaction (system.nodes[0]->wallets.tx_begin_read ());
		account = system.account (transaction, 0);
	}
	auto wallet (std::make_shared<nano_qt::wallet> (*test_application, processor, *system.nodes[0], system.wallet (0), account));
	wallet->start ();
	QTest::mouseClick (wallet->settings_button, Qt::LeftButton);
	nano::raw_key password;
	password.data.clear ();
	system.deadline_set (10s);
	while (password.data == 0)
	{
		ASSERT_NO_ERROR (system.poll ());
		system.wallet (0)->store.password.value (password);
	}
	{
		auto transaction (system.nodes[0]->wallets.tx_begin_read ());
		nano::raw_key password1;
		system.wallet (0)->store.derive_key (password1, transaction, "");
		nano::raw_key password2;
		system.wallet (0)->store.password.value (password2);
		ASSERT_EQ (password1, password2);
	}
	QTest::keyClicks (wallet->settings.new_password, "1");
	QTest::keyClicks (wallet->settings.retype_password, "2");
	QTest::mouseClick (wallet->settings.change, Qt::LeftButton);
	{
		auto transaction (system.nodes[0]->wallets.tx_begin_read ());
		nano::raw_key password1;
		system.wallet (0)->store.derive_key (password1, transaction, "");
		nano::raw_key password2;
		system.wallet (0)->store.password.value (password2);
		ASSERT_EQ (password1, password2);
	}
	ASSERT_EQ ("1", wallet->settings.new_password->text ());
	ASSERT_EQ ("", wallet->settings.retype_password->text ());
}

TEST (wallet, enter_password)
{
	nano_qt::eventloop_processor processor;
	nano::system system (24000, 2);
	nano::account account;
	system.wallet (0)->insert_adhoc (nano::keypair ().prv);
	{
		auto transaction (system.nodes[0]->wallets.tx_begin_read ());
		account = system.account (transaction, 0);
	}
	auto wallet (std::make_shared<nano_qt::wallet> (*test_application, processor, *system.nodes[0], system.wallet (0), account));
	wallet->start ();
	ASSERT_NE (-1, wallet->settings.layout->indexOf (wallet->settings.password));
	ASSERT_NE (-1, wallet->settings.layout->indexOf (wallet->settings.lock_toggle));
	ASSERT_NE (-1, wallet->settings.layout->indexOf (wallet->settings.back));
	// The wallet UI always starts as locked, so we lock it then unlock it again to update the UI.
	// This should never be a problem in actual use, as in reality, the wallet does start locked.
	QTest::mouseClick (wallet->settings.lock_toggle, Qt::LeftButton);
	QTest::mouseClick (wallet->settings.lock_toggle, Qt::LeftButton);
	test_application->processEvents ();
	ASSERT_EQ ("Status: Wallet password empty, Blocks: 1", wallet->status->text ().toStdString ());
	{
		auto transaction (system.nodes[0]->wallets.tx_begin_write ());
		ASSERT_FALSE (system.wallet (0)->store.rekey (transaction, "abc"));
	}
	QTest::mouseClick (wallet->settings_button, Qt::LeftButton);
	QTest::mouseClick (wallet->settings.lock_toggle, Qt::LeftButton);
	test_application->processEvents ();
	ASSERT_EQ ("Status: Wallet locked, Blocks: 1", wallet->status->text ().toStdString ());
	wallet->settings.new_password->setText ("");
	QTest::keyClicks (wallet->settings.password, "abc");
	QTest::mouseClick (wallet->settings.lock_toggle, Qt::LeftButton);
	test_application->processEvents ();
	ASSERT_EQ ("Status: Running, Blocks: 1", wallet->status->text ().toStdString ());
	ASSERT_EQ ("", wallet->settings.password->text ());
}

TEST (wallet, send)
{
	nano_qt::eventloop_processor processor;
	nano::system system (24000, 2);
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	nano::public_key key1 (system.wallet (1)->insert_adhoc (nano::keypair ().prv));
	auto account (nano::test_genesis_key.pub);
	auto wallet (std::make_shared<nano_qt::wallet> (*test_application, processor, *system.nodes[0], system.wallet (0), account));
	wallet->start ();
	ASSERT_NE (wallet->rendering_ratio, nano::raw_ratio);
	QTest::mouseClick (wallet->send_blocks, Qt::LeftButton);
	QTest::keyClicks (wallet->send_account, key1.to_account ().c_str ());
	QTest::keyClicks (wallet->send_count, "2.03");
	QTest::mouseClick (wallet->send_blocks_send, Qt::LeftButton);
	system.deadline_set (10s);
	while (wallet->node.balance (key1).is_zero ())
	{
		ASSERT_NO_ERROR (system.poll ());
	}
	nano::uint128_t amount (wallet->node.balance (key1));
	ASSERT_EQ (2 * wallet->rendering_ratio + (3 * wallet->rendering_ratio / 100), amount);
	QTest::mouseClick (wallet->send_blocks_back, Qt::LeftButton);
	QTest::mouseClick (wallet->show_advanced, Qt::LeftButton);
	QTest::mouseClick (wallet->advanced.show_ledger, Qt::LeftButton);
	QTest::mouseClick (wallet->advanced.ledger_refresh, Qt::LeftButton);
	ASSERT_EQ (2, wallet->advanced.ledger_model->rowCount ());
	ASSERT_EQ (3, wallet->advanced.ledger_model->columnCount ());
	auto item (wallet->advanced.ledger_model->itemFromIndex (wallet->advanced.ledger_model->index (0, 1)));
	auto other_item (wallet->advanced.ledger_model->itemFromIndex (wallet->advanced.ledger_model->index (1, 1)));
	// this seems somewhat random
	ASSERT_TRUE (("2" == item->text ()) || ("2" == other_item->text ()));
}

TEST (wallet, send_locked)
{
	nano_qt::eventloop_processor processor;
	nano::system system (24000, 1);
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	nano::keypair key1;
	{
		auto transaction (system.wallet (0)->wallets.tx_begin_write ());
		system.wallet (0)->enter_password (transaction, "0");
	}
	auto account (nano::test_genesis_key.pub);
	auto wallet (std::make_shared<nano_qt::wallet> (*test_application, processor, *system.nodes[0], system.wallet (0), account));
	wallet->start ();
	QTest::mouseClick (wallet->send_blocks, Qt::LeftButton);
	QTest::keyClicks (wallet->send_account, key1.pub.to_account ().c_str ());
	QTest::keyClicks (wallet->send_count, "2");
	QTest::mouseClick (wallet->send_blocks_send, Qt::LeftButton);
	system.deadline_set (10s);
	while (!wallet->send_blocks_send->isEnabled ())
	{
		test_application->processEvents ();
		ASSERT_NO_ERROR (system.poll ());
	}
}

TEST (wallet, process_block)
{
	nano_qt::eventloop_processor processor;
	nano::system system (24000, 1);
	nano::account account;
	nano::block_hash latest (system.nodes[0]->latest (nano::genesis_account));
	system.wallet (0)->insert_adhoc (nano::keypair ().prv);
	{
		auto transaction (system.nodes[0]->wallets.tx_begin_read ());
		account = system.account (transaction, 0);
	}
	auto wallet (std::make_shared<nano_qt::wallet> (*test_application, processor, *system.nodes[0], system.wallet (0), account));
	wallet->start ();
	ASSERT_EQ ("Process", wallet->block_entry.process->text ());
	ASSERT_EQ ("Back", wallet->block_entry.back->text ());
	nano::keypair key1;
	ASSERT_EQ (wallet->entry_window, wallet->main_stack->currentWidget ());
	QTest::mouseClick (wallet->show_advanced, Qt::LeftButton);
	QTest::mouseClick (wallet->advanced.enter_block, Qt::LeftButton);
	ASSERT_EQ (wallet->block_entry.window, wallet->main_stack->currentWidget ());
	nano::send_block send (latest, key1.pub, 0, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (latest));
	std::string previous;
	send.hashables.previous.encode_hex (previous);
	std::string balance;
	send.hashables.balance.encode_hex (balance);
	std::string signature;
	send.signature.encode_hex (signature);
	std::string block_json;
	send.serialize_json (block_json);
	block_json.erase (std::remove (block_json.begin (), block_json.end (), '\n'), block_json.end ());
	QTest::keyClicks (wallet->block_entry.block, QString::fromStdString (block_json));
	QTest::mouseClick (wallet->block_entry.process, Qt::LeftButton);
	{
		auto transaction (system.nodes[0]->store.tx_begin_read ());
		system.deadline_set (10s);
		while (system.nodes[0]->store.block_exists (transaction, send.hash ()))
		{
			ASSERT_NO_ERROR (system.poll ());
		}
	}
	QTest::mouseClick (wallet->block_entry.back, Qt::LeftButton);
	ASSERT_EQ (wallet->advanced.window, wallet->main_stack->currentWidget ());
}

TEST (wallet, create_send)
{
	nano_qt::eventloop_processor processor;
	nano::keypair key;
	nano::system system (24000, 1);
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	system.wallet (0)->insert_adhoc (key.prv);
	auto account (nano::test_genesis_key.pub);
	auto wallet (std::make_shared<nano_qt::wallet> (*test_application, processor, *system.nodes[0], system.wallet (0), account));
	wallet->start ();
	wallet->client_window->show ();
	QTest::mouseClick (wallet->show_advanced, Qt::LeftButton);
	QTest::mouseClick (wallet->advanced.create_block, Qt::LeftButton);
	QTest::mouseClick (wallet->block_creation.send, Qt::LeftButton);
	QTest::keyClicks (wallet->block_creation.account, nano::test_genesis_key.pub.to_account ().c_str ());
	QTest::keyClicks (wallet->block_creation.amount, "100000000000000000000");
	QTest::keyClicks (wallet->block_creation.destination, key.pub.to_account ().c_str ());
	QTest::mouseClick (wallet->block_creation.create, Qt::LeftButton);
	std::string json (wallet->block_creation.block->toPlainText ().toStdString ());
	ASSERT_FALSE (json.empty ());
	boost::property_tree::ptree tree1;
	std::stringstream istream (json);
	boost::property_tree::read_json (istream, tree1);
	bool error (false);
	nano::state_block send (error, tree1);
	ASSERT_FALSE (error);
	ASSERT_EQ (nano::process_result::progress, system.nodes[0]->process (send).code);
	ASSERT_EQ (nano::process_result::old, system.nodes[0]->process (send).code);
}

TEST (wallet, create_open_receive)
{
	nano_qt::eventloop_processor processor;
	nano::keypair key;
	nano::system system (24000, 1);
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	system.wallet (0)->send_action (nano::test_genesis_key.pub, key.pub, 100);
	nano::block_hash latest1 (system.nodes[0]->latest (nano::test_genesis_key.pub));
	system.wallet (0)->send_action (nano::test_genesis_key.pub, key.pub, 100);
	nano::block_hash latest2 (system.nodes[0]->latest (nano::test_genesis_key.pub));
	ASSERT_NE (latest1, latest2);
	system.wallet (0)->insert_adhoc (key.prv);
	auto account (nano::test_genesis_key.pub);
	auto wallet (std::make_shared<nano_qt::wallet> (*test_application, processor, *system.nodes[0], system.wallet (0), account));
	wallet->start ();
	wallet->client_window->show ();
	QTest::mouseClick (wallet->show_advanced, Qt::LeftButton);
	QTest::mouseClick (wallet->advanced.create_block, Qt::LeftButton);
	wallet->block_creation.open->click ();
	QTest::keyClicks (wallet->block_creation.source, latest1.to_string ().c_str ());
	QTest::keyClicks (wallet->block_creation.representative, nano::test_genesis_key.pub.to_account ().c_str ());
	QTest::mouseClick (wallet->block_creation.create, Qt::LeftButton);
	std::string json1 (wallet->block_creation.block->toPlainText ().toStdString ());
	ASSERT_FALSE (json1.empty ());
	boost::property_tree::ptree tree1;
	std::stringstream istream1 (json1);
	boost::property_tree::read_json (istream1, tree1);
	bool error (false);
	nano::state_block open (error, tree1);
	ASSERT_FALSE (error);
	ASSERT_EQ (nano::process_result::progress, system.nodes[0]->process (open).code);
	ASSERT_EQ (nano::process_result::old, system.nodes[0]->process (open).code);
	wallet->block_creation.block->clear ();
	wallet->block_creation.source->clear ();
	QTest::mouseClick (wallet->block_creation.receive, Qt::LeftButton);
	QTest::keyClicks (wallet->block_creation.source, latest2.to_string ().c_str ());
	QTest::mouseClick (wallet->block_creation.create, Qt::LeftButton);
	std::string json2 (wallet->block_creation.block->toPlainText ().toStdString ());
	ASSERT_FALSE (json2.empty ());
	boost::property_tree::ptree tree2;
	std::stringstream istream2 (json2);
	boost::property_tree::read_json (istream2, tree2);
	bool error2 (false);
	nano::state_block receive (error2, tree2);
	ASSERT_FALSE (error2);
	ASSERT_EQ (nano::process_result::progress, system.nodes[0]->process (receive).code);
	ASSERT_EQ (nano::process_result::old, system.nodes[0]->process (receive).code);
}

TEST (wallet, create_change)
{
	nano_qt::eventloop_processor processor;
	nano::keypair key;
	nano::system system (24000, 1);
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	auto account (nano::test_genesis_key.pub);
	auto wallet (std::make_shared<nano_qt::wallet> (*test_application, processor, *system.nodes[0], system.wallet (0), account));
	wallet->start ();
	wallet->client_window->show ();
	QTest::mouseClick (wallet->show_advanced, Qt::LeftButton);
	QTest::mouseClick (wallet->advanced.create_block, Qt::LeftButton);
	QTest::mouseClick (wallet->block_creation.change, Qt::LeftButton);
	QTest::keyClicks (wallet->block_creation.account, nano::test_genesis_key.pub.to_account ().c_str ());
	QTest::keyClicks (wallet->block_creation.representative, key.pub.to_account ().c_str ());
	QTest::mouseClick (wallet->block_creation.create, Qt::LeftButton);
	std::string json (wallet->block_creation.block->toPlainText ().toStdString ());
	ASSERT_FALSE (json.empty ());
	boost::property_tree::ptree tree1;
	std::stringstream istream (json);
	boost::property_tree::read_json (istream, tree1);
	bool error (false);
	nano::state_block change (error, tree1);
	ASSERT_FALSE (error);
	ASSERT_EQ (nano::process_result::progress, system.nodes[0]->process (change).code);
	ASSERT_EQ (nano::process_result::old, system.nodes[0]->process (change).code);
}

TEST (history, short_text)
{
	nano_qt::eventloop_processor processor;
	nano::keypair key;
	nano::system system (24000, 1);
	system.wallet (0)->insert_adhoc (key.prv);
	nano::account account;
	{
		auto transaction (system.nodes[0]->wallets.tx_begin_read ());
		account = system.account (transaction, 0);
	}
	auto wallet (std::make_shared<nano_qt::wallet> (*test_application, processor, *system.nodes[0], system.wallet (0), account));
	nano::mdb_store store (system.nodes[0]->logger, nano::unique_path ());
	ASSERT_TRUE (!store.init_error ());
	nano::genesis genesis;
	nano::ledger ledger (store, system.nodes[0]->stats);
	{
		auto transaction (store.tx_begin_write ());
		store.initialize (transaction, genesis, ledger.rep_weights);
		nano::keypair key;
		auto latest (ledger.latest (transaction, nano::test_genesis_key.pub));
		nano::send_block send (latest, nano::test_genesis_key.pub, 0, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (latest));
		ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, send).code);
		nano::receive_block receive (send.hash (), send.hash (), nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (send.hash ()));
		ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, receive).code);
		nano::change_block change (receive.hash (), key.pub, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (receive.hash ()));
		ASSERT_EQ (nano::process_result::progress, ledger.process (transaction, change).code);
	}
	nano_qt::history history (ledger, nano::test_genesis_key.pub, *wallet);
	history.refresh ();
	ASSERT_EQ (4, history.model->rowCount ());
}

TEST (wallet, startup_work)
{
	nano_qt::eventloop_processor processor;
	nano::keypair key;
	nano::system system (24000, 1);
	system.wallet (0)->insert_adhoc (key.prv);
	nano::account account;
	{
		auto transaction (system.nodes[0]->wallets.tx_begin_read ());
		account = system.account (transaction, 0);
	}
	auto wallet (std::make_shared<nano_qt::wallet> (*test_application, processor, *system.nodes[0], system.wallet (0), account));
	wallet->start ();
	QTest::mouseClick (wallet->show_advanced, Qt::LeftButton);
	uint64_t work1;
	{
		auto transaction (system.nodes[0]->wallets.tx_begin_read ());
		ASSERT_TRUE (wallet->wallet_m->store.work_get (transaction, nano::test_genesis_key.pub, work1));
	}
	QTest::mouseClick (wallet->accounts_button, Qt::LeftButton);
	QTest::keyClicks (wallet->accounts.account_key_line, "34F0A37AAD20F4A260F0A5B3CB3D7FB50673212263E58A380BC10474BB039CE4");
	QTest::mouseClick (wallet->accounts.account_key_button, Qt::LeftButton);
	system.deadline_set (10s);
	auto again (true);
	while (again)
	{
		ASSERT_NO_ERROR (system.poll ());
		auto transaction (system.nodes[0]->wallets.tx_begin_read ());
		again = wallet->wallet_m->store.work_get (transaction, nano::test_genesis_key.pub, work1);
	}
}

TEST (wallet, block_viewer)
{
	nano_qt::eventloop_processor processor;
	nano::keypair key;
	nano::system system (24000, 1);
	system.wallet (0)->insert_adhoc (key.prv);
	nano::account account;
	{
		auto transaction (system.nodes[0]->wallets.tx_begin_read ());
		account = system.account (transaction, 0);
	}
	auto wallet (std::make_shared<nano_qt::wallet> (*test_application, processor, *system.nodes[0], system.wallet (0), account));
	wallet->start ();
	QTest::mouseClick (wallet->show_advanced, Qt::LeftButton);
	ASSERT_NE (-1, wallet->advanced.layout->indexOf (wallet->advanced.block_viewer));
	QTest::mouseClick (wallet->advanced.block_viewer, Qt::LeftButton);
	ASSERT_EQ (wallet->block_viewer.window, wallet->main_stack->currentWidget ());
	nano::block_hash latest (system.nodes[0]->latest (nano::genesis_account));
	QTest::keyClicks (wallet->block_viewer.hash, latest.to_string ().c_str ());
	QTest::mouseClick (wallet->block_viewer.retrieve, Qt::LeftButton);
	ASSERT_FALSE (wallet->block_viewer.block->toPlainText ().toStdString ().empty ());
	QTest::mouseClick (wallet->block_viewer.back, Qt::LeftButton);
	ASSERT_EQ (wallet->advanced.window, wallet->main_stack->currentWidget ());
}

TEST (wallet, import)
{
	nano_qt::eventloop_processor processor;
	nano::system system (24000, 2);
	std::string json;
	nano::keypair key1;
	nano::keypair key2;
	system.wallet (0)->insert_adhoc (key1.prv);
	{
		auto transaction (system.nodes[0]->wallets.tx_begin_read ());
		system.wallet (0)->store.serialize_json (transaction, json);
	}
	system.wallet (1)->insert_adhoc (key2.prv);
	auto path (nano::unique_path ());
	{
		std::ofstream stream;
		stream.open (path.string ().c_str ());
		stream << json;
	}
	auto wallet (std::make_shared<nano_qt::wallet> (*test_application, processor, *system.nodes[1], system.wallet (1), key2.pub));
	wallet->start ();
	QTest::mouseClick (wallet->show_advanced, Qt::LeftButton);
	ASSERT_EQ (wallet->advanced.window, wallet->main_stack->currentWidget ());
	QTest::mouseClick (wallet->accounts_button, Qt::LeftButton);
	ASSERT_EQ (wallet->accounts.window, wallet->main_stack->currentWidget ());
	QTest::mouseClick (wallet->accounts.import_wallet, Qt::LeftButton);
	ASSERT_EQ (wallet->import.window, wallet->main_stack->currentWidget ());
	QTest::keyClicks (wallet->import.filename, path.string ().c_str ());
	QTest::keyClicks (wallet->import.password, "");
	ASSERT_FALSE (system.wallet (1)->exists (key1.pub));
	QTest::mouseClick (wallet->import.perform, Qt::LeftButton);
	ASSERT_TRUE (system.wallet (1)->exists (key1.pub));
}

TEST (wallet, republish)
{
	nano_qt::eventloop_processor processor;
	nano::system system (24000, 2);
	system.wallet (0)->insert_adhoc (nano::test_genesis_key.prv);
	nano::keypair key;
	nano::block_hash hash;
	{
		auto transaction (system.nodes[0]->store.tx_begin_write ());
		auto latest (system.nodes[0]->ledger.latest (transaction, nano::test_genesis_key.pub));
		nano::send_block block (latest, key.pub, 0, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system.work.generate (latest));
		hash = block.hash ();
		ASSERT_EQ (nano::process_result::progress, system.nodes[0]->ledger.process (transaction, block).code);
	}
	auto account (nano::test_genesis_key.pub);
	auto wallet (std::make_shared<nano_qt::wallet> (*test_application, processor, *system.nodes[0], system.wallet (0), account));
	wallet->start ();
	QTest::mouseClick (wallet->show_advanced, Qt::LeftButton);
	ASSERT_EQ (wallet->advanced.window, wallet->main_stack->currentWidget ());
	QTest::mouseClick (wallet->advanced.block_viewer, Qt::LeftButton);
	ASSERT_EQ (wallet->block_viewer.window, wallet->main_stack->currentWidget ());
	QTest::keyClicks (wallet->block_viewer.hash, hash.to_string ().c_str ());
	QTest::mouseClick (wallet->block_viewer.rebroadcast, Qt::LeftButton);
	ASSERT_FALSE (system.nodes[1]->balance (nano::test_genesis_key.pub).is_zero ());
	system.deadline_set (10s);
	while (system.nodes[1]->balance (nano::test_genesis_key.pub).is_zero ())
	{
		ASSERT_NO_ERROR (system.poll ());
	}
}

TEST (wallet, ignore_empty_adhoc)
{
	nano_qt::eventloop_processor processor;
	nano::system system (24000, 1);
	nano::keypair key1;
	system.wallet (0)->insert_adhoc (key1.prv);
	auto wallet (std::make_shared<nano_qt::wallet> (*test_application, processor, *system.nodes[0], system.wallet (0), key1.pub));
	wallet->start ();
	QTest::mouseClick (wallet->show_advanced, Qt::LeftButton);
	ASSERT_EQ (wallet->advanced.window, wallet->main_stack->currentWidget ());
	QTest::mouseClick (wallet->accounts_button, Qt::LeftButton);
	ASSERT_EQ (wallet->accounts.window, wallet->main_stack->currentWidget ());
	QTest::keyClicks (wallet->accounts.account_key_line, nano::test_genesis_key.prv.data.to_string ().c_str ());
	QTest::mouseClick (wallet->accounts.account_key_button, Qt::LeftButton);
	ASSERT_EQ (1, wallet->accounts.model->rowCount ());
	ASSERT_EQ (0, wallet->accounts.account_key_line->text ().length ());
	nano::keypair key;
	QTest::keyClicks (wallet->accounts.account_key_line, key.prv.data.to_string ().c_str ());
	QTest::mouseClick (wallet->accounts.account_key_button, Qt::LeftButton);
	ASSERT_EQ (1, wallet->accounts.model->rowCount ());
	ASSERT_EQ (0, wallet->accounts.account_key_line->text ().length ());
	QTest::mouseClick (wallet->accounts.create_account, Qt::LeftButton);
	test_application->processEvents ();
	test_application->processEvents ();
	ASSERT_EQ (2, wallet->accounts.model->rowCount ());
}

TEST (wallet, change_seed)
{
	nano_qt::eventloop_processor processor;
	nano::system system (24000, 1);
	auto key1 (system.wallet (0)->deterministic_insert ());
	system.wallet (0)->deterministic_insert ();
	nano::raw_key seed3;
	{
		auto transaction (system.wallet (0)->wallets.tx_begin_read ());
		system.wallet (0)->store.seed (seed3, transaction);
	}
	auto wallet_key (key1);
	auto wallet (std::make_shared<nano_qt::wallet> (*test_application, processor, *system.nodes[0], system.wallet (0), wallet_key));
	wallet->start ();
	QTest::mouseClick (wallet->show_advanced, Qt::LeftButton);
	ASSERT_EQ (wallet->advanced.window, wallet->main_stack->currentWidget ());
	QTest::mouseClick (wallet->accounts_button, Qt::LeftButton);
	ASSERT_EQ (wallet->accounts.window, wallet->main_stack->currentWidget ());
	QTest::mouseClick (wallet->accounts.import_wallet, Qt::LeftButton);
	ASSERT_EQ (wallet->import.window, wallet->main_stack->currentWidget ());
	nano::raw_key seed;
	seed.data.clear ();
	QTest::keyClicks (wallet->import.seed, seed.data.to_string ().c_str ());
	nano::raw_key seed1;
	{
		auto transaction (system.wallet (0)->wallets.tx_begin_read ());
		system.wallet (0)->store.seed (seed1, transaction);
	}
	ASSERT_NE (seed, seed1);
	ASSERT_TRUE (system.wallet (0)->exists (key1));
	ASSERT_EQ (2, wallet->accounts.model->rowCount ());
	QTest::mouseClick (wallet->import.import_seed, Qt::LeftButton);
	ASSERT_EQ (2, wallet->accounts.model->rowCount ());
	QTest::keyClicks (wallet->import.clear_line, "clear keys");
	QTest::mouseClick (wallet->import.import_seed, Qt::LeftButton);
	ASSERT_EQ (1, wallet->accounts.model->rowCount ());
	ASSERT_TRUE (wallet->import.clear_line->text ().toStdString ().empty ());
	nano::raw_key seed2;
	auto transaction (system.wallet (0)->wallets.tx_begin_read ());
	system.wallet (0)->store.seed (seed2, transaction);
	ASSERT_EQ (seed, seed2);
	ASSERT_FALSE (system.wallet (0)->exists (key1));
	ASSERT_NE (key1, wallet->account);
	auto key2 (wallet->account);
	ASSERT_TRUE (system.wallet (0)->exists (key2));
	QTest::keyClicks (wallet->import.seed, seed3.data.to_string ().c_str ());
	QTest::keyClicks (wallet->import.clear_line, "clear keys");
	QTest::mouseClick (wallet->import.import_seed, Qt::LeftButton);
	ASSERT_EQ (key1, wallet->account);
	ASSERT_FALSE (system.wallet (0)->exists (key2));
	ASSERT_TRUE (system.wallet (0)->exists (key1));
}

TEST (wallet, seed_work_generation)
{
	nano_qt::eventloop_processor processor;
	nano::system system (24000, 1);
	auto key1 (system.wallet (0)->deterministic_insert ());
	auto wallet (std::make_shared<nano_qt::wallet> (*test_application, processor, *system.nodes[0], system.wallet (0), key1));
	wallet->start ();
	QTest::mouseClick (wallet->show_advanced, Qt::LeftButton);
	ASSERT_EQ (wallet->advanced.window, wallet->main_stack->currentWidget ());
	QTest::mouseClick (wallet->accounts_button, Qt::LeftButton);
	ASSERT_EQ (wallet->accounts.window, wallet->main_stack->currentWidget ());
	QTest::mouseClick (wallet->accounts.import_wallet, Qt::LeftButton);
	ASSERT_EQ (wallet->import.window, wallet->main_stack->currentWidget ());
	nano::raw_key seed;
	nano::uint256_union prv;
	nano::deterministic_key (seed.data, 0, prv);
	nano::uint256_union pub (nano::pub_key (prv));
	QTest::keyClicks (wallet->import.seed, seed.data.to_string ().c_str ());
	QTest::keyClicks (wallet->import.clear_line, "clear keys");
	uint64_t work (0);
	QTest::mouseClick (wallet->import.import_seed, Qt::LeftButton);
	system.deadline_set (10s);
	while (work == 0)
	{
		auto ec = system.poll ();
		auto transaction (system.wallet (0)->wallets.tx_begin_read ());
		system.wallet (0)->store.work_get (transaction, pub, work);
		ASSERT_NO_ERROR (ec);
	}
	auto transaction (system.nodes[0]->store.tx_begin_read ());
	ASSERT_FALSE (nano::work_validate (system.nodes[0]->ledger.latest_root (transaction, pub), work));
}

TEST (wallet, backup_seed)
{
	nano_qt::eventloop_processor processor;
	nano::system system (24000, 1);
	auto key1 (system.wallet (0)->deterministic_insert ());
	auto wallet (std::make_shared<nano_qt::wallet> (*test_application, processor, *system.nodes[0], system.wallet (0), key1));
	wallet->start ();
	QTest::mouseClick (wallet->show_advanced, Qt::LeftButton);
	ASSERT_EQ (wallet->advanced.window, wallet->main_stack->currentWidget ());
	QTest::mouseClick (wallet->accounts_button, Qt::LeftButton);
	ASSERT_EQ (wallet->accounts.window, wallet->main_stack->currentWidget ());
	QTest::mouseClick (wallet->accounts.backup_seed, Qt::LeftButton);
	nano::raw_key seed;
	auto transaction (system.wallet (0)->wallets.tx_begin_read ());
	system.wallet (0)->store.seed (seed, transaction);
	ASSERT_EQ (seed.data.to_string (), test_application->clipboard ()->text ().toStdString ());
}

TEST (wallet, import_locked)
{
	nano_qt::eventloop_processor processor;
	nano::system system (24000, 1);
	auto key1 (system.wallet (0)->deterministic_insert ());
	{
		auto transaction (system.wallet (0)->wallets.tx_begin_write ());
		system.wallet (0)->store.rekey (transaction, "1");
	}
	auto wallet (std::make_shared<nano_qt::wallet> (*test_application, processor, *system.nodes[0], system.wallet (0), key1));
	wallet->start ();
	QTest::mouseClick (wallet->show_advanced, Qt::LeftButton);
	ASSERT_EQ (wallet->advanced.window, wallet->main_stack->currentWidget ());
	QTest::mouseClick (wallet->accounts_button, Qt::LeftButton);
	ASSERT_EQ (wallet->accounts.window, wallet->main_stack->currentWidget ());
	nano::raw_key seed1;
	seed1.data.clear ();
	QTest::keyClicks (wallet->import.seed, seed1.data.to_string ().c_str ());
	QTest::keyClicks (wallet->import.clear_line, "clear keys");
	{
		auto transaction (system.wallet (0)->wallets.tx_begin_write ());
		system.wallet (0)->enter_password (transaction, "");
	}
	QTest::mouseClick (wallet->import.import_seed, Qt::LeftButton);
	nano::raw_key seed2;
	{
		auto transaction (system.wallet (0)->wallets.tx_begin_write ());
		system.wallet (0)->store.seed (seed2, transaction);
		ASSERT_NE (seed1, seed2);
		system.wallet (0)->enter_password (transaction, "1");
	}
	QTest::mouseClick (wallet->import.import_seed, Qt::LeftButton);
	nano::raw_key seed3;
	auto transaction (system.wallet (0)->wallets.tx_begin_read ());
	system.wallet (0)->store.seed (seed3, transaction);
	ASSERT_EQ (seed1, seed3);
}
// DISABLED: this always fails
TEST (wallet, DISABLED_synchronizing)
{
	nano_qt::eventloop_processor processor;
	nano::system system0 (24000, 1);
	nano::system system1 (24001, 1);
	auto key1 (system0.wallet (0)->deterministic_insert ());
	auto wallet (std::make_shared<nano_qt::wallet> (*test_application, processor, *system0.nodes[0], system0.wallet (0), key1));
	wallet->start ();
	{
		auto transaction (system1.nodes[0]->store.tx_begin_write ());
		auto latest (system1.nodes[0]->ledger.latest (transaction, nano::genesis_account));
		nano::send_block send (latest, key1, 0, nano::test_genesis_key.prv, nano::test_genesis_key.pub, system1.work.generate (latest));
		system1.nodes[0]->ledger.process (transaction, send);
	}
	ASSERT_EQ (0, wallet->active_status.active.count (nano_qt::status_types::synchronizing));
	system0.nodes[0]->bootstrap_initiator.bootstrap (system1.nodes[0]->network.endpoint ());
	system1.deadline_set (10s);
	while (wallet->active_status.active.count (nano_qt::status_types::synchronizing) == 0)
	{
		ASSERT_NO_ERROR (system0.poll ());
		ASSERT_NO_ERROR (system1.poll ());
		test_application->processEvents ();
	}
	system1.deadline_set (25s);
	while (wallet->active_status.active.count (nano_qt::status_types::synchronizing) == 1)
	{
		ASSERT_NO_ERROR (system0.poll ());
		ASSERT_NO_ERROR (system1.poll ());
		test_application->processEvents ();
	}
}
