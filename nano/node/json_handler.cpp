#include <nano/lib/config.hpp>
#include <nano/lib/json_error_response.hpp>
#include <nano/lib/timer.hpp>
#include <nano/node/common.hpp>
#include <nano/node/ipc.hpp>
#include <nano/node/json_handler.hpp>
#include <nano/node/json_payment_observer.hpp>
#include <nano/node/node.hpp>
#include <nano/node/node_rpc_config.hpp>

#include <boost/array.hpp>
#include <boost/bind.hpp>
#include <boost/endian/conversion.hpp>
#include <boost/polymorphic_cast.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/thread/thread_time.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>

namespace
{
void construct_json (nano::seq_con_info_component * component, boost::property_tree::ptree & parent);
using ipc_json_handler_no_arg_func_map = std::unordered_map<std::string, std::function<void(nano::json_handler *)>>;
ipc_json_handler_no_arg_func_map create_ipc_json_handler_no_arg_func_map ();
auto ipc_json_handler_no_arg_funcs = create_ipc_json_handler_no_arg_func_map ();
bool block_confirmed (nano::node & node, nano::transaction & transaction, nano::block_hash const & hash, bool include_active, bool include_only_confirmed);
}

nano::json_handler::json_handler (nano::node & node_a, nano::node_rpc_config const & node_rpc_config_a, std::string const & body_a, std::function<void(std::string const &)> const & response_a, std::function<void()> stop_callback_a) :
body (body_a),
node (node_a),
response (response_a),
stop_callback (stop_callback_a),
node_rpc_config (node_rpc_config_a)
{
}

void nano::json_handler::process_request (bool unsafe_a)
{
	try
	{
		std::stringstream istream (body);
		boost::property_tree::read_json (istream, request);
		action = request.get<std::string> ("action");
		auto no_arg_func_iter = ipc_json_handler_no_arg_funcs.find (action);
		if (no_arg_func_iter != ipc_json_handler_no_arg_funcs.cend ())
		{
			// First try the map of options with no arguments
			no_arg_func_iter->second (this);
		}
		else
		{
			// Try the rest of the options
			if (action == "wallet_seed")
			{
				if (unsafe_a || node.network_params.network.is_test_network ())
				{
					wallet_seed ();
				}
				else
				{
					json_error_response (response, "Unsafe RPC not allowed");
				}
			}
			else if (action == "chain")
			{
				chain ();
			}
			else if (action == "successors")
			{
				chain (true);
			}
			else if (action == "history")
			{
				request.put ("head", request.get<std::string> ("hash"));
				account_history ();
			}
			else if (action == "knano_from_raw" || action == "krai_from_raw")
			{
				mnano_from_raw (nano::kxrb_ratio);
			}
			else if (action == "knano_to_raw" || action == "krai_to_raw")
			{
				mnano_to_raw (nano::kxrb_ratio);
			}
			else if (action == "nano_from_raw" || action == "rai_from_raw")
			{
				mnano_from_raw (nano::xrb_ratio);
			}
			else if (action == "nano_to_raw" || action == "rai_to_raw")
			{
				mnano_to_raw (nano::xrb_ratio);
			}
			else if (action == "mnano_from_raw" || action == "mrai_from_raw")
			{
				mnano_from_raw ();
			}
			else if (action == "mnano_to_raw" || action == "mrai_to_raw")
			{
				mnano_to_raw ();
			}
			else if (action == "password_valid")
			{
				password_valid ();
			}
			else if (action == "wallet_locked")
			{
				password_valid (true);
			}
			else
			{
				json_error_response (response, "Unknown command");
			}
		}
	}
	catch (std::runtime_error const &)
	{
		json_error_response (response, "Unable to parse JSON");
	}
	catch (...)
	{
		json_error_response (response, "Internal server error in RPC");
	}
}

void nano::json_handler::response_errors ()
{
	if (ec || response_l.empty ())
	{
		boost::property_tree::ptree response_error;
		response_error.put ("error", ec ? ec.message () : "Empty response");
		std::stringstream ostream;
		boost::property_tree::write_json (ostream, response_error);
		response (ostream.str ());
	}
	else
	{
		std::stringstream ostream;
		boost::property_tree::write_json (ostream, response_l);
		response (ostream.str ());
	}
}

std::shared_ptr<nano::wallet> nano::json_handler::wallet_impl ()
{
	if (!ec)
	{
		std::string wallet_text (request.get<std::string> ("wallet"));
		nano::uint256_union wallet;
		if (!wallet.decode_hex (wallet_text))
		{
			auto existing (node.wallets.items.find (wallet));
			if (existing != node.wallets.items.end ())
			{
				return existing->second;
			}
			else
			{
				ec = nano::error_common::wallet_not_found;
			}
		}
		else
		{
			ec = nano::error_common::bad_wallet_number;
		}
	}
	return nullptr;
}

bool nano::json_handler::wallet_locked_impl (nano::transaction const & transaction_a, std::shared_ptr<nano::wallet> wallet_a)
{
	bool result (false);
	if (!ec)
	{
		if (!wallet_a->store.valid_password (transaction_a))
		{
			ec = nano::error_common::wallet_locked;
			result = true;
		}
	}
	return result;
}

bool nano::json_handler::wallet_account_impl (nano::transaction const & transaction_a, std::shared_ptr<nano::wallet> wallet_a, nano::account const & account_a)
{
	bool result (false);
	if (!ec)
	{
		if (wallet_a->store.find (transaction_a, account_a) != wallet_a->store.end ())
		{
			result = true;
		}
		else
		{
			ec = nano::error_common::account_not_found_wallet;
		}
	}
	return result;
}

nano::account nano::json_handler::account_impl (std::string account_text, std::error_code ec_a)
{
	nano::account result (0);
	if (!ec)
	{
		if (account_text.empty ())
		{
			account_text = request.get<std::string> ("account");
		}
		if (result.decode_account (account_text))
		{
			ec = ec_a;
		}
		else if (account_text[3] == '-' || account_text[4] == '-')
		{
			// nano- and xrb- prefixes are deprecated
			response_l.put ("deprecated_account_format", "1");
		}
	}
	return result;
}

nano::amount nano::json_handler::amount_impl ()
{
	nano::amount result (0);
	if (!ec)
	{
		std::string amount_text (request.get<std::string> ("amount"));
		if (result.decode_dec (amount_text))
		{
			ec = nano::error_common::invalid_amount;
		}
	}
	return result;
}

std::shared_ptr<nano::block> nano::json_handler::block_impl (bool signature_work_required)
{
	std::shared_ptr<nano::block> result;
	if (!ec)
	{
		std::string block_text (request.get<std::string> ("block"));
		boost::property_tree::ptree block_l;
		std::stringstream block_stream (block_text);
		boost::property_tree::read_json (block_stream, block_l);
		if (!signature_work_required)
		{
			block_l.put ("signature", "0");
			block_l.put ("work", "0");
		}
		result = nano::deserialize_block_json (block_l);
		if (result == nullptr)
		{
			ec = nano::error_blocks::invalid_block;
		}
	}
	return result;
}

std::shared_ptr<nano::block> nano::json_handler::block_json_impl (bool signature_work_required)
{
	std::shared_ptr<nano::block> result;
	if (!ec)
	{
		auto block_l (request.get_child ("block"));
		if (!signature_work_required)
		{
			block_l.put ("signature", "0");
			block_l.put ("work", "0");
		}
		result = nano::deserialize_block_json (block_l);
		if (result == nullptr)
		{
			ec = nano::error_blocks::invalid_block;
		}
	}
	return result;
}

nano::block_hash nano::json_handler::hash_impl (std::string search_text)
{
	nano::block_hash result (0);
	if (!ec)
	{
		std::string hash_text (request.get<std::string> (search_text));
		if (result.decode_hex (hash_text))
		{
			ec = nano::error_blocks::invalid_block_hash;
		}
	}
	return result;
}

nano::amount nano::json_handler::threshold_optional_impl ()
{
	nano::amount result (0);
	boost::optional<std::string> threshold_text (request.get_optional<std::string> ("threshold"));
	if (!ec && threshold_text.is_initialized ())
	{
		if (result.decode_dec (threshold_text.get ()))
		{
			ec = nano::error_common::bad_threshold;
		}
	}
	return result;
}

uint64_t nano::json_handler::work_optional_impl ()
{
	uint64_t result (0);
	boost::optional<std::string> work_text (request.get_optional<std::string> ("work"));
	if (!ec && work_text.is_initialized ())
	{
		if (nano::from_string_hex (work_text.get (), result))
		{
			ec = nano::error_common::bad_work_format;
		}
	}
	return result;
}

uint64_t nano::json_handler::difficulty_optional_impl ()
{
	uint64_t difficulty (node.network_params.network.publish_threshold);
	boost::optional<std::string> difficulty_text (request.get_optional<std::string> ("difficulty"));
	if (!ec && difficulty_text.is_initialized ())
	{
		if (nano::from_string_hex (difficulty_text.get (), difficulty))
		{
			ec = nano::error_rpc::bad_difficulty_format;
		}
	}
	return difficulty;
}

double nano::json_handler::multiplier_optional_impl (uint64_t & difficulty)
{
	double multiplier (1.);
	boost::optional<std::string> multiplier_text (request.get_optional<std::string> ("multiplier"));
	if (!ec && multiplier_text.is_initialized ())
	{
		auto success = boost::conversion::try_lexical_convert<double> (multiplier_text.get (), multiplier);
		if (success && multiplier > 0.)
		{
			difficulty = nano::difficulty::from_multiplier (multiplier, node.network_params.network.publish_threshold);
		}
		else
		{
			ec = nano::error_rpc::bad_multiplier_format;
		}
	}
	return multiplier;
}

namespace
{
bool decode_unsigned (std::string const & text, uint64_t & number)
{
	bool result;
	size_t end;
	try
	{
		number = std::stoull (text, &end);
		result = false;
	}
	catch (std::invalid_argument const &)
	{
		result = true;
	}
	catch (std::out_of_range const &)
	{
		result = true;
	}
	result = result || end != text.size ();
	return result;
}
}

uint64_t nano::json_handler::count_impl ()
{
	uint64_t result (0);
	if (!ec)
	{
		std::string count_text (request.get<std::string> ("count"));
		if (decode_unsigned (count_text, result) || result == 0)
		{
			ec = nano::error_common::invalid_count;
		}
	}
	return result;
}

uint64_t nano::json_handler::count_optional_impl (uint64_t result)
{
	boost::optional<std::string> count_text (request.get_optional<std::string> ("count"));
	if (!ec && count_text.is_initialized ())
	{
		if (decode_unsigned (count_text.get (), result))
		{
			ec = nano::error_common::invalid_count;
		}
	}
	return result;
}

uint64_t nano::json_handler::offset_optional_impl (uint64_t result)
{
	boost::optional<std::string> offset_text (request.get_optional<std::string> ("offset"));
	if (!ec && offset_text.is_initialized ())
	{
		if (decode_unsigned (offset_text.get (), result))
		{
			ec = nano::error_rpc::invalid_offset;
		}
	}
	return result;
}

void nano::json_handler::account_balance ()
{
	auto account (account_impl ());
	if (!ec)
	{
		auto balance (node.balance_pending (account));
		response_l.put ("balance", balance.first.convert_to<std::string> ());
		response_l.put ("pending", balance.second.convert_to<std::string> ());
	}
	response_errors ();
}

void nano::json_handler::account_block_count ()
{
	auto account (account_impl ());
	if (!ec)
	{
		auto transaction (node.store.tx_begin_read ());
		nano::account_info info;
		if (!node.store.account_get (transaction, account, info))
		{
			response_l.put ("block_count", std::to_string (info.block_count));
		}
		else
		{
			ec = nano::error_common::account_not_found;
		}
	}
	response_errors ();
}

void nano::json_handler::account_create ()
{
	auto rpc_l (shared_from_this ());
	node.worker.push_task ([rpc_l]() {
		auto wallet (rpc_l->wallet_impl ());
		if (!rpc_l->ec)
		{
			const bool generate_work = rpc_l->request.get<bool> ("work", true);
			nano::account new_key;
			auto index_text (rpc_l->request.get_optional<std::string> ("index"));
			if (index_text.is_initialized ())
			{
				uint64_t index;
				if (decode_unsigned (index_text.get (), index) || index > static_cast<uint64_t> (std::numeric_limits<uint32_t>::max ()))
				{
					rpc_l->ec = nano::error_common::invalid_index;
				}
				else
				{
					new_key = wallet->deterministic_insert (static_cast<uint32_t> (index), generate_work);
				}
			}
			else
			{
				new_key = wallet->deterministic_insert (generate_work);
			}

			if (!rpc_l->ec)
			{
				if (!new_key.is_zero ())
				{
					rpc_l->response_l.put ("account", new_key.to_account ());
				}
				else
				{
					rpc_l->ec = nano::error_common::wallet_locked;
				}
			}
		}
		rpc_l->response_errors ();
	});
}

void nano::json_handler::account_get ()
{
	std::string key_text (request.get<std::string> ("key"));
	nano::uint256_union pub;
	if (!pub.decode_hex (key_text))
	{
		response_l.put ("account", pub.to_account ());
	}
	else
	{
		ec = nano::error_common::bad_public_key;
	}
	response_errors ();
}

void nano::json_handler::account_info ()
{
	auto account (account_impl ());
	if (!ec)
	{
		const bool representative = request.get<bool> ("representative", false);
		const bool weight = request.get<bool> ("weight", false);
		const bool pending = request.get<bool> ("pending", false);
		auto transaction (node.store.tx_begin_read ());
		nano::account_info info;
		uint64_t confirmation_height;
		auto error = node.store.account_get (transaction, account, info) | node.store.confirmation_height_get (transaction, account, confirmation_height);
		if (!error)
		{
			response_l.put ("frontier", info.head.to_string ());
			response_l.put ("open_block", info.open_block.to_string ());
			response_l.put ("representative_block", info.rep_block.to_string ());
			std::string balance;
			nano::uint128_union (info.balance).encode_dec (balance);
			response_l.put ("balance", balance);
			response_l.put ("modified_timestamp", std::to_string (info.modified));
			response_l.put ("block_count", std::to_string (info.block_count));
			response_l.put ("account_version", info.epoch == nano::epoch::epoch_1 ? "1" : "0");
			response_l.put ("confirmation_height", std::to_string (confirmation_height));
			if (representative)
			{
				auto block (node.store.block_get (transaction, info.rep_block));
				assert (block != nullptr);
				response_l.put ("representative", block->representative ().to_account ());
			}
			if (weight)
			{
				auto account_weight (node.ledger.weight (transaction, account));
				response_l.put ("weight", account_weight.convert_to<std::string> ());
			}
			if (pending)
			{
				auto account_pending (node.ledger.account_pending (transaction, account));
				response_l.put ("pending", account_pending.convert_to<std::string> ());
			}
		}
		else
		{
			ec = nano::error_common::account_not_found;
		}
	}
	response_errors ();
}

void nano::json_handler::account_key ()
{
	auto account (account_impl ());
	if (!ec)
	{
		response_l.put ("key", account.to_string ());
	}
	response_errors ();
}

void nano::json_handler::account_list ()
{
	auto wallet (wallet_impl ());
	if (!ec)
	{
		boost::property_tree::ptree accounts;
		auto transaction (node.wallets.tx_begin_read ());
		for (auto i (wallet->store.begin (transaction)), j (wallet->store.end ()); i != j; ++i)
		{
			boost::property_tree::ptree entry;
			entry.put ("", nano::account (i->first).to_account ());
			accounts.push_back (std::make_pair ("", entry));
		}
		response_l.add_child ("accounts", accounts);
	}
	response_errors ();
}

void nano::json_handler::account_move ()
{
	auto rpc_l (shared_from_this ());
	node.worker.push_task ([rpc_l]() {
		auto wallet (rpc_l->wallet_impl ());
		if (!rpc_l->ec)
		{
			std::string source_text (rpc_l->request.get<std::string> ("source"));
			auto accounts_text (rpc_l->request.get_child ("accounts"));
			nano::uint256_union source;
			if (!source.decode_hex (source_text))
			{
				auto existing (rpc_l->node.wallets.items.find (source));
				if (existing != rpc_l->node.wallets.items.end ())
				{
					auto source (existing->second);
					std::vector<nano::public_key> accounts;
					for (auto i (accounts_text.begin ()), n (accounts_text.end ()); i != n; ++i)
					{
						auto account (rpc_l->account_impl (i->second.get<std::string> ("")));
						accounts.push_back (account);
					}
					auto transaction (rpc_l->node.wallets.tx_begin_write ());
					auto error (wallet->store.move (transaction, source->store, accounts));
					rpc_l->response_l.put ("moved", error ? "0" : "1");
				}
				else
				{
					rpc_l->ec = nano::error_rpc::source_not_found;
				}
			}
			else
			{
				rpc_l->ec = nano::error_rpc::bad_source;
			}
		}
		rpc_l->response_errors ();
	});
}

void nano::json_handler::account_remove ()
{
	auto rpc_l (shared_from_this ());
	node.worker.push_task ([rpc_l]() {
		auto wallet (rpc_l->wallet_impl ());
		auto account (rpc_l->account_impl ());
		if (!rpc_l->ec)
		{
			auto transaction (rpc_l->node.wallets.tx_begin_write ());
			rpc_l->wallet_locked_impl (transaction, wallet);
			rpc_l->wallet_account_impl (transaction, wallet, account);
			if (!rpc_l->ec)
			{
				wallet->store.erase (transaction, account);
				rpc_l->response_l.put ("removed", "1");
			}
		}
		rpc_l->response_errors ();
	});
}

void nano::json_handler::account_representative ()
{
	auto account (account_impl ());
	if (!ec)
	{
		auto transaction (node.store.tx_begin_read ());
		nano::account_info info;
		if (!node.store.account_get (transaction, account, info))
		{
			auto block (node.store.block_get (transaction, info.rep_block));
			assert (block != nullptr);
			response_l.put ("representative", block->representative ().to_account ());
		}
		else
		{
			ec = nano::error_common::account_not_found;
		}
	}
	response_errors ();
}

void nano::json_handler::account_representative_set ()
{
	auto rpc_l (shared_from_this ());
	node.worker.push_task ([rpc_l]() {
		auto wallet (rpc_l->wallet_impl ());
		auto account (rpc_l->account_impl ());
		std::string representative_text (rpc_l->request.get<std::string> ("representative"));
		auto representative (rpc_l->account_impl (representative_text, nano::error_rpc::bad_representative_number));
		if (!rpc_l->ec)
		{
			auto work (rpc_l->work_optional_impl ());
			if (!rpc_l->ec && work)
			{
				auto transaction (rpc_l->node.wallets.tx_begin_write ());
				rpc_l->wallet_locked_impl (transaction, wallet);
				rpc_l->wallet_account_impl (transaction, wallet, account);
				if (!rpc_l->ec)
				{
					nano::account_info info;
					auto block_transaction (rpc_l->node.store.tx_begin_read ());
					if (!rpc_l->node.store.account_get (block_transaction, account, info))
					{
						if (nano::work_validate (info.head, work))
						{
							rpc_l->ec = nano::error_common::invalid_work;
						}
					}
					else
					{
						rpc_l->ec = nano::error_common::account_not_found;
					}
				}
			}
			if (!rpc_l->ec)
			{
				bool generate_work (work == 0); // Disable work generation if "work" option is provided
				auto response_a (rpc_l->response);
				auto response_data (std::make_shared<boost::property_tree::ptree> (rpc_l->response_l));
				// clang-format off
				wallet->change_async(account, representative, [response_a, response_data](std::shared_ptr<nano::block> block) {
					if (block != nullptr)
					{
						response_data->put("block", block->hash().to_string());
						std::stringstream ostream;
						boost::property_tree::write_json(ostream, *response_data);
						response_a(ostream.str());
					}
					else
					{
						json_error_response(response_a, "Error generating block");
					}
				},
					work, generate_work);
				// clang-format on
			}
		}
		// Because of change_async
		if (rpc_l->ec)
		{
			rpc_l->response_errors ();
		}
	});
}

void nano::json_handler::account_weight ()
{
	auto account (account_impl ());
	if (!ec)
	{
		auto balance (node.weight (account));
		response_l.put ("weight", balance.convert_to<std::string> ());
	}
	response_errors ();
}

void nano::json_handler::accounts_balances ()
{
	boost::property_tree::ptree balances;
	for (auto & accounts : request.get_child ("accounts"))
	{
		auto account (account_impl (accounts.second.data ()));
		if (!ec)
		{
			boost::property_tree::ptree entry;
			auto balance (node.balance_pending (account));
			entry.put ("balance", balance.first.convert_to<std::string> ());
			entry.put ("pending", balance.second.convert_to<std::string> ());
			balances.push_back (std::make_pair (account.to_account (), entry));
		}
	}
	response_l.add_child ("balances", balances);
	response_errors ();
}

void nano::json_handler::accounts_create ()
{
	auto rpc_l (shared_from_this ());
	node.worker.push_task ([rpc_l]() {
		auto wallet (rpc_l->wallet_impl ());
		auto count (rpc_l->count_impl ());
		if (!rpc_l->ec)
		{
			const bool generate_work = rpc_l->request.get<bool> ("work", false);
			boost::property_tree::ptree accounts;
			for (auto i (0); accounts.size () < count; ++i)
			{
				nano::account new_key (wallet->deterministic_insert (generate_work));
				if (!new_key.is_zero ())
				{
					boost::property_tree::ptree entry;
					entry.put ("", new_key.to_account ());
					accounts.push_back (std::make_pair ("", entry));
				}
			}
			rpc_l->response_l.add_child ("accounts", accounts);
		}
		rpc_l->response_errors ();
	});
}

void nano::json_handler::accounts_frontiers ()
{
	boost::property_tree::ptree frontiers;
	auto transaction (node.store.tx_begin_read ());
	for (auto & accounts : request.get_child ("accounts"))
	{
		auto account (account_impl (accounts.second.data ()));
		if (!ec)
		{
			auto latest (node.ledger.latest (transaction, account));
			if (!latest.is_zero ())
			{
				frontiers.put (account.to_account (), latest.to_string ());
			}
		}
	}
	response_l.add_child ("frontiers", frontiers);
	response_errors ();
}

void nano::json_handler::accounts_pending ()
{
	auto count (count_optional_impl ());
	auto threshold (threshold_optional_impl ());
	const bool source = request.get<bool> ("source", false);
	const bool include_active = request.get<bool> ("include_active", false);
	const bool include_only_confirmed = request.get<bool> ("include_only_confirmed", false);
	const bool sorting = request.get<bool> ("sorting", false);
	auto simple (threshold.is_zero () && !source && !sorting); // if simple, response is a list of hashes for each account
	boost::property_tree::ptree pending;
	auto transaction (node.store.tx_begin_read ());
	for (auto & accounts : request.get_child ("accounts"))
	{
		auto account (account_impl (accounts.second.data ()));
		if (!ec)
		{
			boost::property_tree::ptree peers_l;
			for (auto i (node.store.pending_begin (transaction, nano::pending_key (account, 0))); nano::pending_key (i->first).account == account && peers_l.size () < count; ++i)
			{
				nano::pending_key const & key (i->first);
				if (block_confirmed (node, transaction, key.hash, include_active, include_only_confirmed))
				{
					if (simple)
					{
						boost::property_tree::ptree entry;
						entry.put ("", key.hash.to_string ());
						peers_l.push_back (std::make_pair ("", entry));
					}
					else
					{
						nano::pending_info const & info (i->second);
						if (info.amount.number () >= threshold.number ())
						{
							if (source)
							{
								boost::property_tree::ptree pending_tree;
								pending_tree.put ("amount", info.amount.number ().convert_to<std::string> ());
								pending_tree.put ("source", info.source.to_account ());
								peers_l.add_child (key.hash.to_string (), pending_tree);
							}
							else
							{
								peers_l.put (key.hash.to_string (), info.amount.number ().convert_to<std::string> ());
							}
						}
					}
				}
			}
			if (sorting && !simple)
			{
				if (source)
				{
					peers_l.sort ([](const auto & child1, const auto & child2) -> bool {
						return child1.second.template get<nano::uint128_t> ("amount") > child2.second.template get<nano::uint128_t> ("amount");
					});
				}
				else
				{
					peers_l.sort ([](const auto & child1, const auto & child2) -> bool {
						return child1.second.template get<nano::uint128_t> ("") > child2.second.template get<nano::uint128_t> ("");
					});
				}
			}
			pending.add_child (account.to_account (), peers_l);
		}
	}
	response_l.add_child ("blocks", pending);
	response_errors ();
}

void nano::json_handler::active_difficulty ()
{
	auto include_trend (request.get<bool> ("include_trend", false));
	response_l.put ("network_minimum", nano::to_string_hex (node.network_params.network.publish_threshold));
	auto difficulty_active = node.active.active_difficulty ();
	response_l.put ("network_current", nano::to_string_hex (difficulty_active));
	auto multiplier = nano::difficulty::to_multiplier (difficulty_active, node.network_params.network.publish_threshold);
	response_l.put ("multiplier", nano::to_string (multiplier));
	if (include_trend)
	{
		boost::property_tree::ptree trend_entry_l;
		auto trend_l (node.active.difficulty_trend ());
		for (auto multiplier_l : trend_l)
		{
			boost::property_tree::ptree entry;
			entry.put ("", nano::to_string (multiplier_l));
			trend_entry_l.push_back (std::make_pair ("", entry));
		}
		response_l.add_child ("difficulty_trend", trend_entry_l);
	}
	response_errors ();
}

void nano::json_handler::available_supply ()
{
	auto genesis_balance (node.balance (node.network_params.ledger.genesis_account)); // Cold storage genesis
	auto landing_balance (node.balance (nano::account ("059F68AAB29DE0D3A27443625C7EA9CDDB6517A8B76FE37727EF6A4D76832AD5"))); // Active unavailable account
	auto faucet_balance (node.balance (nano::account ("8E319CE6F3025E5B2DF66DA7AB1467FE48F1679C13DD43BFDB29FA2E9FC40D3B"))); // Faucet account
	auto burned_balance ((node.balance_pending (nano::account (0))).second); // Burning 0 account
	auto available (node.network_params.ledger.genesis_amount - genesis_balance - landing_balance - faucet_balance - burned_balance);
	response_l.put ("available", available.convert_to<std::string> ());
	response_errors ();
}

void state_subtype (nano::transaction const & transaction_a, nano::node & node_a, std::shared_ptr<nano::block> block_a, nano::uint128_t const & balance_a, boost::property_tree::ptree & tree_a)
{
	// Subtype check
	auto previous_balance (node_a.ledger.balance (transaction_a, block_a->previous ()));
	if (balance_a < previous_balance)
	{
		tree_a.put ("subtype", "send");
	}
	else
	{
		if (block_a->link ().is_zero ())
		{
			tree_a.put ("subtype", "change");
		}
		else if (balance_a == previous_balance && !node_a.ledger.epoch_link.is_zero () && node_a.ledger.is_epoch_link (block_a->link ()))
		{
			tree_a.put ("subtype", "epoch");
		}
		else
		{
			tree_a.put ("subtype", "receive");
		}
	}
}

void nano::json_handler::block_info ()
{
	auto hash (hash_impl ());
	if (!ec)
	{
		nano::block_sideband sideband;
		auto transaction (node.store.tx_begin_read ());
		auto block (node.store.block_get (transaction, hash, &sideband));
		if (block != nullptr)
		{
			nano::account account (block->account ().is_zero () ? sideband.account : block->account ());
			response_l.put ("block_account", account.to_account ());
			auto amount (node.ledger.amount (transaction, hash));
			response_l.put ("amount", amount.convert_to<std::string> ());
			auto balance (node.ledger.balance (transaction, hash));
			response_l.put ("balance", balance.convert_to<std::string> ());
			response_l.put ("height", std::to_string (sideband.height));
			response_l.put ("local_timestamp", std::to_string (sideband.timestamp));
			auto confirmed (node.block_confirmed_or_being_confirmed (transaction, hash));
			response_l.put ("confirmed", confirmed);

			bool json_block_l = request.get<bool> ("json_block", false);
			if (json_block_l)
			{
				boost::property_tree::ptree block_node_l;
				block->serialize_json (block_node_l);
				response_l.add_child ("contents", block_node_l);
			}
			else
			{
				std::string contents;
				block->serialize_json (contents);
				response_l.put ("contents", contents);
			}
			if (block->type () == nano::block_type::state)
			{
				state_subtype (transaction, node, block, balance, response_l);
			}
		}
		else
		{
			ec = nano::error_blocks::not_found;
		}
	}
	response_errors ();
}

void nano::json_handler::block_confirm ()
{
	auto hash (hash_impl ());
	if (!ec)
	{
		auto transaction (node.store.tx_begin_read ());
		auto block_l (node.store.block_get (transaction, hash));
		if (block_l != nullptr)
		{
			if (!node.block_confirmed_or_being_confirmed (transaction, hash))
			{
				// Start new confirmation for unconfirmed block
				node.block_confirm (std::move (block_l));
			}
			else
			{
				// Add record in confirmation history for confirmed block
				nano::election_status status{ block_l, 0, std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::system_clock::now ().time_since_epoch ()), std::chrono::duration_values<std::chrono::milliseconds>::zero (), nano::election_status_type::active_confirmation_height };
				{
					std::lock_guard<std::mutex> lock (node.active.mutex);
					node.active.confirmed.push_back (status);
					if (node.active.confirmed.size () > node.config.confirmation_history_size)
					{
						node.active.confirmed.pop_front ();
					}
				}
				// Trigger callback for confirmed block
				node.block_arrival.add (hash);
				auto account (node.ledger.account (transaction, hash));
				auto amount (node.ledger.amount (transaction, hash));
				bool is_state_send (false);
				if (auto state = dynamic_cast<nano::state_block *> (block_l.get ()))
				{
					is_state_send = node.ledger.is_send (transaction, *state);
				}
				node.observers.blocks.notify (status, account, amount, is_state_send);
			}
			response_l.put ("started", "1");
		}
		else
		{
			ec = nano::error_blocks::not_found;
		}
	}
	response_errors ();
}

void nano::json_handler::blocks ()
{
	const bool json_block_l = request.get<bool> ("json_block", false);
	boost::property_tree::ptree blocks;
	auto transaction (node.store.tx_begin_read ());
	for (boost::property_tree::ptree::value_type & hashes : request.get_child ("hashes"))
	{
		if (!ec)
		{
			std::string hash_text = hashes.second.data ();
			nano::uint256_union hash;
			if (!hash.decode_hex (hash_text))
			{
				auto block (node.store.block_get (transaction, hash));
				if (block != nullptr)
				{
					if (json_block_l)
					{
						boost::property_tree::ptree block_node_l;
						block->serialize_json (block_node_l);
						blocks.add_child (hash_text, block_node_l);
					}
					else
					{
						std::string contents;
						block->serialize_json (contents);
						blocks.put (hash_text, contents);
					}
				}
				else
				{
					ec = nano::error_blocks::not_found;
				}
			}
			else
			{
				ec = nano::error_blocks::bad_hash_number;
			}
		}
	}
	response_l.add_child ("blocks", blocks);
	response_errors ();
}

void nano::json_handler::blocks_info ()
{
	const bool pending = request.get<bool> ("pending", false);
	const bool source = request.get<bool> ("source", false);
	const bool json_block_l = request.get<bool> ("json_block", false);
	const bool include_not_found = request.get<bool> ("include_not_found", false);

	boost::property_tree::ptree blocks;
	boost::property_tree::ptree blocks_not_found;
	auto transaction (node.store.tx_begin_read ());
	for (boost::property_tree::ptree::value_type & hashes : request.get_child ("hashes"))
	{
		if (!ec)
		{
			std::string hash_text = hashes.second.data ();
			nano::uint256_union hash;
			if (!hash.decode_hex (hash_text))
			{
				nano::block_sideband sideband;
				auto block (node.store.block_get (transaction, hash, &sideband));
				if (block != nullptr)
				{
					boost::property_tree::ptree entry;
					nano::account account (block->account ().is_zero () ? sideband.account : block->account ());
					entry.put ("block_account", account.to_account ());
					auto amount (node.ledger.amount (transaction, hash));
					entry.put ("amount", amount.convert_to<std::string> ());
					auto balance (node.ledger.balance (transaction, hash));
					entry.put ("balance", balance.convert_to<std::string> ());
					entry.put ("height", std::to_string (sideband.height));
					entry.put ("local_timestamp", std::to_string (sideband.timestamp));
					auto confirmed (node.block_confirmed_or_being_confirmed (transaction, hash));
					entry.put ("confirmed", confirmed);

					if (json_block_l)
					{
						boost::property_tree::ptree block_node_l;
						block->serialize_json (block_node_l);
						entry.add_child ("contents", block_node_l);
					}
					else
					{
						std::string contents;
						block->serialize_json (contents);
						entry.put ("contents", contents);
					}
					if (block->type () == nano::block_type::state)
					{
						state_subtype (transaction, node, block, balance, entry);
					}
					if (pending)
					{
						bool exists (false);
						auto destination (node.ledger.block_destination (transaction, *block));
						if (!destination.is_zero ())
						{
							exists = node.store.pending_exists (transaction, nano::pending_key (destination, hash));
						}
						entry.put ("pending", exists ? "1" : "0");
					}
					if (source)
					{
						nano::block_hash source_hash (node.ledger.block_source (transaction, *block));
						auto block_a (node.store.block_get (transaction, source_hash));
						if (block_a != nullptr)
						{
							auto source_account (node.ledger.account (transaction, source_hash));
							entry.put ("source_account", source_account.to_account ());
						}
						else
						{
							entry.put ("source_account", "0");
						}
					}
					blocks.push_back (std::make_pair (hash_text, entry));
				}
				else if (include_not_found)
				{
					boost::property_tree::ptree entry;
					entry.put ("", hash_text);
					blocks_not_found.push_back (std::make_pair ("", entry));
				}
				else
				{
					ec = nano::error_blocks::not_found;
				}
			}
			else
			{
				ec = nano::error_blocks::bad_hash_number;
			}
		}
	}
	if (!ec)
	{
		response_l.add_child ("blocks", blocks);
		if (include_not_found)
		{
			response_l.add_child ("blocks_not_found", blocks_not_found);
		}
	}
	response_errors ();
}

void nano::json_handler::block_account ()
{
	auto hash (hash_impl ());
	if (!ec)
	{
		auto transaction (node.store.tx_begin_read ());
		if (node.store.block_exists (transaction, hash))
		{
			auto account (node.ledger.account (transaction, hash));
			response_l.put ("account", account.to_account ());
		}
		else
		{
			ec = nano::error_blocks::not_found;
		}
	}
	response_errors ();
}

void nano::json_handler::block_count ()
{
	auto transaction (node.store.tx_begin_read ());
	response_l.put ("count", std::to_string (node.store.block_count (transaction).sum ()));
	response_l.put ("unchecked", std::to_string (node.store.unchecked_count (transaction)));

	const auto include_cemented = request.get<bool> ("include_cemented", false);
	if (include_cemented)
	{
		response_l.put ("cemented", std::to_string (node.store.cemented_count (transaction)));
	}

	response_errors ();
}

void nano::json_handler::block_count_type ()
{
	auto transaction (node.store.tx_begin_read ());
	nano::block_counts count (node.store.block_count (transaction));
	response_l.put ("send", std::to_string (count.send));
	response_l.put ("receive", std::to_string (count.receive));
	response_l.put ("open", std::to_string (count.open));
	response_l.put ("change", std::to_string (count.change));
	response_l.put ("state_v0", std::to_string (count.state_v0));
	response_l.put ("state_v1", std::to_string (count.state_v1));
	response_l.put ("state", std::to_string (count.state_v0 + count.state_v1));
	response_errors ();
}

void nano::json_handler::block_create ()
{
	if (!ec)
	{
		std::string type (request.get<std::string> ("type"));
		nano::uint256_union wallet (0);
		boost::optional<std::string> wallet_text (request.get_optional<std::string> ("wallet"));
		if (wallet_text.is_initialized ())
		{
			if (wallet.decode_hex (wallet_text.get ()))
			{
				ec = nano::error_common::bad_wallet_number;
			}
		}
		nano::account account (0);
		boost::optional<std::string> account_text (request.get_optional<std::string> ("account"));
		if (!ec && account_text.is_initialized ())
		{
			account = account_impl (account_text.get ());
		}
		nano::uint256_union representative (0);
		boost::optional<std::string> representative_text (request.get_optional<std::string> ("representative"));
		if (!ec && representative_text.is_initialized ())
		{
			representative = account_impl (representative_text.get (), nano::error_rpc::bad_representative_number);
		}
		nano::uint256_union destination (0);
		boost::optional<std::string> destination_text (request.get_optional<std::string> ("destination"));
		if (!ec && destination_text.is_initialized ())
		{
			destination = account_impl (destination_text.get (), nano::error_rpc::bad_destination);
		}
		nano::block_hash source (0);
		boost::optional<std::string> source_text (request.get_optional<std::string> ("source"));
		if (!ec && source_text.is_initialized ())
		{
			if (source.decode_hex (source_text.get ()))
			{
				ec = nano::error_rpc::bad_source;
			}
		}
		nano::uint128_union amount (0);
		boost::optional<std::string> amount_text (request.get_optional<std::string> ("amount"));
		if (!ec && amount_text.is_initialized ())
		{
			if (amount.decode_dec (amount_text.get ()))
			{
				ec = nano::error_common::invalid_amount;
			}
		}
		auto work (work_optional_impl ());
		nano::raw_key prv;
		prv.data.clear ();
		nano::uint256_union previous (0);
		nano::uint128_union balance (0);
		if (!ec && wallet != 0 && account != 0)
		{
			auto existing (node.wallets.items.find (wallet));
			if (existing != node.wallets.items.end ())
			{
				auto transaction (node.wallets.tx_begin_read ());
				auto block_transaction (node.store.tx_begin_read ());
				wallet_locked_impl (transaction, existing->second);
				wallet_account_impl (transaction, existing->second, account);
				if (!ec)
				{
					existing->second->store.fetch (transaction, account, prv);
					previous = node.ledger.latest (block_transaction, account);
					balance = node.ledger.account_balance (block_transaction, account);
				}
			}
			else
			{
				ec = nano::error_common::wallet_not_found;
			}
		}
		boost::optional<std::string> key_text (request.get_optional<std::string> ("key"));
		if (!ec && key_text.is_initialized ())
		{
			if (prv.data.decode_hex (key_text.get ()))
			{
				ec = nano::error_common::bad_private_key;
			}
		}
		boost::optional<std::string> previous_text (request.get_optional<std::string> ("previous"));
		if (!ec && previous_text.is_initialized ())
		{
			if (previous.decode_hex (previous_text.get ()))
			{
				ec = nano::error_rpc::bad_previous;
			}
		}
		boost::optional<std::string> balance_text (request.get_optional<std::string> ("balance"));
		if (!ec && balance_text.is_initialized ())
		{
			if (balance.decode_dec (balance_text.get ()))
			{
				ec = nano::error_rpc::invalid_balance;
			}
		}
		nano::uint256_union link (0);
		boost::optional<std::string> link_text (request.get_optional<std::string> ("link"));
		if (!ec && link_text.is_initialized ())
		{
			if (link.decode_account (link_text.get ()))
			{
				if (link.decode_hex (link_text.get ()))
				{
					ec = nano::error_rpc::bad_link;
				}
			}
		}
		else
		{
			// Retrieve link from source or destination
			link = source.is_zero () ? destination : source;
		}
		if (!ec)
		{
			if (prv.data != 0)
			{
				nano::uint256_union pub (nano::pub_key (prv.data));
				// Fetching account balance & previous for send blocks (if aren't given directly)
				if (!previous_text.is_initialized () && !balance_text.is_initialized ())
				{
					auto transaction (node.store.tx_begin_read ());
					previous = node.ledger.latest (transaction, pub);
					balance = node.ledger.account_balance (transaction, pub);
				}
				// Double check current balance if previous block is specified
				else if (previous_text.is_initialized () && balance_text.is_initialized () && type == "send")
				{
					auto transaction (node.store.tx_begin_read ());
					if (node.store.block_exists (transaction, previous) && node.store.block_balance (transaction, previous) != balance.number ())
					{
						ec = nano::error_rpc::block_create_balance_mismatch;
					}
				}
				// Check for incorrect account key
				if (!ec && account_text.is_initialized ())
				{
					if (account != pub)
					{
						ec = nano::error_rpc::block_create_public_key_mismatch;
					}
				}
				if (type == "state")
				{
					if (previous_text.is_initialized () && !representative.is_zero () && (!link.is_zero () || link_text.is_initialized ()))
					{
						if (work == 0)
						{
							work = node.work_generate_blocking (previous.is_zero () ? pub : previous);
						}
						nano::state_block state (pub, previous, representative, balance, link, prv, pub, work);
						response_l.put ("hash", state.hash ().to_string ());
						bool json_block_l = request.get<bool> ("json_block", false);
						if (json_block_l)
						{
							boost::property_tree::ptree block_node_l;
							state.serialize_json (block_node_l);
							response_l.add_child ("block", block_node_l);
						}
						else
						{
							std::string contents;
							state.serialize_json (contents);
							response_l.put ("block", contents);
						}
					}
					else
					{
						ec = nano::error_rpc::block_create_requirements_state;
					}
				}
				else if (type == "open")
				{
					if (representative != 0 && source != 0)
					{
						if (work == 0)
						{
							work = node.work_generate_blocking (pub);
						}
						nano::open_block open (source, representative, pub, prv, pub, work);
						response_l.put ("hash", open.hash ().to_string ());
						std::string contents;
						open.serialize_json (contents);
						response_l.put ("block", contents);
					}
					else
					{
						ec = nano::error_rpc::block_create_requirements_open;
					}
				}
				else if (type == "receive")
				{
					if (source != 0 && previous != 0)
					{
						if (work == 0)
						{
							work = node.work_generate_blocking (previous);
						}
						nano::receive_block receive (previous, source, prv, pub, work);
						response_l.put ("hash", receive.hash ().to_string ());
						std::string contents;
						receive.serialize_json (contents);
						response_l.put ("block", contents);
					}
					else
					{
						ec = nano::error_rpc::block_create_requirements_receive;
					}
				}
				else if (type == "change")
				{
					if (representative != 0 && previous != 0)
					{
						if (work == 0)
						{
							work = node.work_generate_blocking (previous);
						}
						nano::change_block change (previous, representative, prv, pub, work);
						response_l.put ("hash", change.hash ().to_string ());
						std::string contents;
						change.serialize_json (contents);
						response_l.put ("block", contents);
					}
					else
					{
						ec = nano::error_rpc::block_create_requirements_change;
					}
				}
				else if (type == "send")
				{
					if (destination != 0 && previous != 0 && balance != 0 && amount != 0)
					{
						if (balance.number () >= amount.number ())
						{
							if (work == 0)
							{
								work = node.work_generate_blocking (previous);
							}
							nano::send_block send (previous, destination, balance.number () - amount.number (), prv, pub, work);
							response_l.put ("hash", send.hash ().to_string ());
							std::string contents;
							send.serialize_json (contents);
							response_l.put ("block", contents);
						}
						else
						{
							ec = nano::error_common::insufficient_balance;
						}
					}
					else
					{
						ec = nano::error_rpc::block_create_requirements_send;
					}
				}
				else
				{
					ec = nano::error_blocks::invalid_type;
				}
			}
			else
			{
				ec = nano::error_rpc::block_create_key_required;
			}
		}
	}
	response_errors ();
}

void nano::json_handler::block_hash ()
{
	const bool json_block_l = request.get<bool> ("json_block", false);
	std::shared_ptr<nano::block> block;
	if (json_block_l)
	{
		block = block_json_impl (true);
	}
	else
	{
		block = block_impl (true);
	}

	if (!ec)
	{
		response_l.put ("hash", block->hash ().to_string ());
	}
	response_errors ();
}

void nano::json_handler::bootstrap ()
{
	std::string address_text = request.get<std::string> ("address");
	std::string port_text = request.get<std::string> ("port");
	boost::system::error_code address_ec;
	auto address (boost::asio::ip::address_v6::from_string (address_text, address_ec));
	if (!address_ec)
	{
		uint16_t port;
		if (!nano::parse_port (port_text, port))
		{
			if (!node.flags.disable_legacy_bootstrap)
			{
				node.bootstrap_initiator.bootstrap (nano::endpoint (address, port));
				response_l.put ("success", "");
			}
			else
			{
				ec = nano::error_rpc::disabled_bootstrap_legacy;
			}
		}
		else
		{
			ec = nano::error_common::invalid_port;
		}
	}
	else
	{
		ec = nano::error_common::invalid_ip_address;
	}
	response_errors ();
}

void nano::json_handler::bootstrap_any ()
{
	if (!node.flags.disable_legacy_bootstrap)
	{
		node.bootstrap_initiator.bootstrap ();
		response_l.put ("success", "");
	}
	else
	{
		ec = nano::error_rpc::disabled_bootstrap_legacy;
	}
	response_errors ();
}

void nano::json_handler::bootstrap_lazy ()
{
	auto hash (hash_impl ());
	const bool force = request.get<bool> ("force", false);
	if (!ec)
	{
		if (!node.flags.disable_lazy_bootstrap)
		{
			node.bootstrap_initiator.bootstrap_lazy (hash, force);
			response_l.put ("started", "1");
		}
		else
		{
			ec = nano::error_rpc::disabled_bootstrap_lazy;
		}
	}
	response_errors ();
}

/*
 * @warning This is an internal/diagnostic RPC, do not rely on its interface being stable
 */
void nano::json_handler::bootstrap_status ()
{
	auto attempt (node.bootstrap_initiator.current_attempt ());
	if (attempt != nullptr)
	{
		response_l.put ("clients", std::to_string (attempt->clients.size ()));
		response_l.put ("pulls", std::to_string (attempt->pulls.size ()));
		response_l.put ("pulling", std::to_string (attempt->pulling));
		response_l.put ("connections", std::to_string (attempt->connections));
		response_l.put ("idle", std::to_string (attempt->idle.size ()));
		response_l.put ("target_connections", std::to_string (attempt->target_connections (attempt->pulls.size ())));
		response_l.put ("total_blocks", std::to_string (attempt->total_blocks));
		response_l.put ("runs_count", std::to_string (attempt->runs_count));
		std::string mode_text;
		if (attempt->mode == nano::bootstrap_mode::legacy)
		{
			mode_text = "legacy";
		}
		else if (attempt->mode == nano::bootstrap_mode::lazy)
		{
			mode_text = "lazy";
		}
		else if (attempt->mode == nano::bootstrap_mode::wallet_lazy)
		{
			mode_text = "wallet_lazy";
		}
		response_l.put ("mode", mode_text);
		response_l.put ("lazy_blocks", std::to_string (attempt->lazy_blocks.size ()));
		response_l.put ("lazy_state_unknown", std::to_string (attempt->lazy_state_unknown.size ()));
		response_l.put ("lazy_balances", std::to_string (attempt->lazy_balances.size ()));
		response_l.put ("lazy_pulls", std::to_string (attempt->lazy_pulls.size ()));
		response_l.put ("lazy_stopped", std::to_string (attempt->lazy_stopped));
		response_l.put ("lazy_keys", std::to_string (attempt->lazy_keys.size ()));
		if (!attempt->lazy_keys.empty ())
		{
			response_l.put ("lazy_key_1", (*(attempt->lazy_keys.begin ())).to_string ());
		}
	}
	else
	{
		response_l.put ("active", "0");
	}
	response_errors ();
}

void nano::json_handler::chain (bool successors)
{
	successors = successors != request.get<bool> ("reverse", false);
	auto hash (hash_impl ("block"));
	auto count (count_impl ());
	auto offset (offset_optional_impl (0));
	if (!ec)
	{
		boost::property_tree::ptree blocks;
		auto transaction (node.store.tx_begin_read ());
		while (!hash.is_zero () && blocks.size () < count)
		{
			auto block_l (node.store.block_get (transaction, hash));
			if (block_l != nullptr)
			{
				if (offset > 0)
				{
					--offset;
				}
				else
				{
					boost::property_tree::ptree entry;
					entry.put ("", hash.to_string ());
					blocks.push_back (std::make_pair ("", entry));
				}
				hash = successors ? node.store.block_successor (transaction, hash) : block_l->previous ();
			}
			else
			{
				hash.clear ();
			}
		}
		response_l.add_child ("blocks", blocks);
	}
	response_errors ();
}

void nano::json_handler::confirmation_active ()
{
	uint64_t announcements (0);
	boost::optional<std::string> announcements_text (request.get_optional<std::string> ("announcements"));
	if (announcements_text.is_initialized ())
	{
		announcements = strtoul (announcements_text.get ().c_str (), NULL, 10);
	}
	boost::property_tree::ptree elections;
	{
		std::lock_guard<std::mutex> lock (node.active.mutex);
		for (auto i (node.active.roots.begin ()), n (node.active.roots.end ()); i != n; ++i)
		{
			if (i->election->confirmation_request_count >= announcements && !i->election->confirmed && !i->election->stopped)
			{
				boost::property_tree::ptree entry;
				entry.put ("", i->root.to_string ());
				elections.push_back (std::make_pair ("", entry));
			}
		}
	}
	response_l.add_child ("confirmations", elections);
	response_errors ();
}

void nano::json_handler::confirmation_height_currently_processing ()
{
	auto hash = node.pending_confirmation_height.current ();
	if (!hash.is_zero ())
	{
		response_l.put ("hash", node.pending_confirmation_height.current ().to_string ());
	}
	else
	{
		ec = nano::error_rpc::confirmation_height_not_processing;
	}
	response_errors ();
}

void nano::json_handler::confirmation_history ()
{
	boost::property_tree::ptree elections;
	boost::property_tree::ptree confirmation_stats;
	std::chrono::milliseconds running_total (0);
	nano::block_hash hash (0);
	boost::optional<std::string> hash_text (request.get_optional<std::string> ("hash"));
	if (hash_text.is_initialized ())
	{
		hash = hash_impl ();
	}
	if (!ec)
	{
		auto confirmed (node.active.list_confirmed ());
		for (auto i (confirmed.begin ()), n (confirmed.end ()); i != n; ++i)
		{
			if (hash.is_zero () || i->winner->hash () == hash)
			{
				boost::property_tree::ptree election;
				election.put ("hash", i->winner->hash ().to_string ());
				election.put ("duration", i->election_duration.count ());
				election.put ("time", i->election_end.count ());
				election.put ("tally", i->tally.to_string_dec ());
				elections.push_back (std::make_pair ("", election));
			}
			running_total += i->election_duration;
		}
	}
	confirmation_stats.put ("count", elections.size ());
	if (elections.size () >= 1)
	{
		confirmation_stats.put ("average", (running_total.count ()) / elections.size ());
	}
	response_l.add_child ("confirmation_stats", confirmation_stats);
	response_l.add_child ("confirmations", elections);
	response_errors ();
}

void nano::json_handler::confirmation_info ()
{
	const bool representatives = request.get<bool> ("representatives", false);
	const bool contents = request.get<bool> ("contents", true);
	const bool json_block_l = request.get<bool> ("json_block", false);
	std::string root_text (request.get<std::string> ("root"));
	nano::qualified_root root;
	if (!root.decode_hex (root_text))
	{
		std::lock_guard<std::mutex> lock (node.active.mutex);
		auto conflict_info (node.active.roots.find (root));
		if (conflict_info != node.active.roots.end ())
		{
			response_l.put ("announcements", std::to_string (conflict_info->election->confirmation_request_count));
			auto election (conflict_info->election);
			nano::uint128_t total (0);
			response_l.put ("last_winner", election->status.winner->hash ().to_string ());
			auto transaction (node.store.tx_begin_read ());
			auto tally_l (election->tally (transaction));
			boost::property_tree::ptree blocks;
			for (auto i (tally_l.begin ()), n (tally_l.end ()); i != n; ++i)
			{
				boost::property_tree::ptree entry;
				auto const & tally (i->first);
				entry.put ("tally", tally.convert_to<std::string> ());
				total += tally;
				if (contents)
				{
					if (json_block_l)
					{
						boost::property_tree::ptree block_node_l;
						i->second->serialize_json (block_node_l);
						entry.add_child ("contents", block_node_l);
					}
					else
					{
						std::string contents;
						i->second->serialize_json (contents);
						entry.put ("contents", contents);
					}
				}
				if (representatives)
				{
					std::multimap<nano::uint128_t, nano::account, std::greater<nano::uint128_t>> representatives;
					for (auto ii (election->last_votes.begin ()), nn (election->last_votes.end ()); ii != nn; ++ii)
					{
						if (i->second->hash () == ii->second.hash)
						{
							nano::account const & representative (ii->first);
							auto amount (node.ledger.rep_weights.representation_get (representative));
							representatives.emplace (std::move (amount), representative);
						}
					}
					boost::property_tree::ptree representatives_list;
					for (auto ii (representatives.begin ()), nn (representatives.end ()); ii != nn; ++ii)
					{
						representatives_list.put (ii->second.to_account (), ii->first.convert_to<std::string> ());
					}
					entry.add_child ("representatives", representatives_list);
				}
				blocks.add_child ((i->second->hash ()).to_string (), entry);
			}
			response_l.put ("total_tally", total.convert_to<std::string> ());
			response_l.add_child ("blocks", blocks);
		}
		else
		{
			ec = nano::error_rpc::confirmation_not_found;
		}
	}
	else
	{
		ec = nano::error_rpc::invalid_root;
	}
	response_errors ();
}

void nano::json_handler::confirmation_quorum ()
{
	response_l.put ("quorum_delta", node.delta ().convert_to<std::string> ());
	response_l.put ("online_weight_quorum_percent", std::to_string (node.config.online_weight_quorum));
	response_l.put ("online_weight_minimum", node.config.online_weight_minimum.to_string_dec ());
	response_l.put ("online_stake_total", node.online_reps.online_stake ().convert_to<std::string> ());
	response_l.put ("peers_stake_total", node.rep_crawler.total_weight ().convert_to<std::string> ());
	response_l.put ("peers_stake_required", std::max (node.config.online_weight_minimum.number (), node.delta ()).convert_to<std::string> ());
	if (request.get<bool> ("peer_details", false))
	{
		boost::property_tree::ptree peers;
		for (auto & peer : node.rep_crawler.representatives_by_weight ())
		{
			boost::property_tree::ptree peer_node;
			peer_node.put ("account", peer.account.to_account ());
			peer_node.put ("ip", peer.channel->to_string ());
			peer_node.put ("weight", peer.weight.to_string_dec ());
			peers.push_back (std::make_pair ("", peer_node));
		}
		response_l.add_child ("peers", peers);
	}
	response_errors ();
}

void nano::json_handler::database_txn_tracker ()
{
	boost::property_tree::ptree json;

	if (node.config.diagnostics_config.txn_tracking.enable)
	{
		unsigned min_read_time_milliseconds = 0;
		boost::optional<std::string> min_read_time_text (request.get_optional<std::string> ("min_read_time"));
		if (min_read_time_text.is_initialized ())
		{
			auto success = boost::conversion::try_lexical_convert<unsigned> (*min_read_time_text, min_read_time_milliseconds);
			if (!success)
			{
				ec = nano::error_common::invalid_amount;
			}
		}

		unsigned min_write_time_milliseconds = 0;
		if (!ec)
		{
			boost::optional<std::string> min_write_time_text (request.get_optional<std::string> ("min_write_time"));
			if (min_write_time_text.is_initialized ())
			{
				auto success = boost::conversion::try_lexical_convert<unsigned> (*min_write_time_text, min_write_time_milliseconds);
				if (!success)
				{
					ec = nano::error_common::invalid_amount;
				}
			}
		}

		if (!ec)
		{
			node.store.serialize_mdb_tracker (json, std::chrono::milliseconds (min_read_time_milliseconds), std::chrono::milliseconds (min_write_time_milliseconds));
			response_l.put_child ("txn_tracking", json);
		}
	}
	else
	{
		ec = nano::error_common::tracking_not_enabled;
	}

	response_errors ();
}

void nano::json_handler::delegators ()
{
	auto account (account_impl ());
	if (!ec)
	{
		boost::property_tree::ptree delegators;
		auto transaction (node.store.tx_begin_read ());
		for (auto i (node.store.latest_begin (transaction)), n (node.store.latest_end ()); i != n; ++i)
		{
			nano::account_info const & info (i->second);
			auto block (node.store.block_get (transaction, info.rep_block));
			assert (block != nullptr);
			if (block->representative () == account)
			{
				std::string balance;
				nano::uint128_union (info.balance).encode_dec (balance);
				nano::account const & account (i->first);
				delegators.put (account.to_account (), balance);
			}
		}
		response_l.add_child ("delegators", delegators);
	}
	response_errors ();
}

void nano::json_handler::delegators_count ()
{
	auto account (account_impl ());
	if (!ec)
	{
		uint64_t count (0);
		auto transaction (node.store.tx_begin_read ());
		for (auto i (node.store.latest_begin (transaction)), n (node.store.latest_end ()); i != n; ++i)
		{
			nano::account_info const & info (i->second);
			auto block (node.store.block_get (transaction, info.rep_block));
			assert (block != nullptr);
			if (block->representative () == account)
			{
				++count;
			}
		}
		response_l.put ("count", std::to_string (count));
	}
	response_errors ();
}

void nano::json_handler::deterministic_key ()
{
	std::string seed_text (request.get<std::string> ("seed"));
	std::string index_text (request.get<std::string> ("index"));
	nano::raw_key seed;
	if (!seed.data.decode_hex (seed_text))
	{
		try
		{
			uint32_t index (std::stoul (index_text));
			nano::uint256_union prv;
			nano::deterministic_key (seed.data, index, prv);
			nano::uint256_union pub (nano::pub_key (prv));
			response_l.put ("private", prv.to_string ());
			response_l.put ("public", pub.to_string ());
			response_l.put ("account", pub.to_account ());
		}
		catch (std::logic_error const &)
		{
			ec = nano::error_common::invalid_index;
		}
	}
	else
	{
		ec = nano::error_common::bad_seed;
	}
	response_errors ();
}

void nano::json_handler::frontiers ()
{
	auto start (account_impl ());
	auto count (count_impl ());
	if (!ec)
	{
		boost::property_tree::ptree frontiers;
		auto transaction (node.store.tx_begin_read ());
		for (auto i (node.store.latest_begin (transaction, start)), n (node.store.latest_end ()); i != n && frontiers.size () < count; ++i)
		{
			frontiers.put (i->first.to_account (), i->second.head.to_string ());
		}
		response_l.add_child ("frontiers", frontiers);
	}
	response_errors ();
}

void nano::json_handler::account_count ()
{
	auto transaction (node.store.tx_begin_read ());
	auto size (node.store.account_count (transaction));
	response_l.put ("count", std::to_string (size));
	response_errors ();
}

namespace
{
class history_visitor : public nano::block_visitor
{
public:
	history_visitor (nano::json_handler & handler_a, bool raw_a, nano::transaction & transaction_a, boost::property_tree::ptree & tree_a, nano::block_hash const & hash_a, std::vector<nano::public_key> const & accounts_filter_a) :
	handler (handler_a),
	raw (raw_a),
	transaction (transaction_a),
	tree (tree_a),
	hash (hash_a),
	accounts_filter (accounts_filter_a)
	{
	}
	virtual ~history_visitor () = default;
	void send_block (nano::send_block const & block_a)
	{
		if (should_ignore_account (block_a.hashables.destination))
		{
			return;
		}
		tree.put ("type", "send");
		auto account (block_a.hashables.destination.to_account ());
		tree.put ("account", account);
		auto amount (handler.node.ledger.amount (transaction, hash).convert_to<std::string> ());
		tree.put ("amount", amount);
		if (raw)
		{
			tree.put ("destination", account);
			tree.put ("balance", block_a.hashables.balance.to_string_dec ());
			tree.put ("previous", block_a.hashables.previous.to_string ());
		}
	}
	void receive_block (nano::receive_block const & block_a)
	{
		if (should_ignore_account (block_a.hashables.source))
		{
			return;
		}
		tree.put ("type", "receive");
		auto account (handler.node.ledger.account (transaction, block_a.hashables.source).to_account ());
		tree.put ("account", account);
		auto amount (handler.node.ledger.amount (transaction, hash).convert_to<std::string> ());
		tree.put ("amount", amount);
		if (raw)
		{
			tree.put ("source", block_a.hashables.source.to_string ());
			tree.put ("previous", block_a.hashables.previous.to_string ());
		}
	}
	void open_block (nano::open_block const & block_a)
	{
		if (should_ignore_account (block_a.hashables.source))
		{
			return;
		}
		if (raw)
		{
			tree.put ("type", "open");
			tree.put ("representative", block_a.hashables.representative.to_account ());
			tree.put ("source", block_a.hashables.source.to_string ());
			tree.put ("opened", block_a.hashables.account.to_account ());
		}
		else
		{
			// Report opens as a receive
			tree.put ("type", "receive");
		}
		if (block_a.hashables.source != network_params.ledger.genesis_account)
		{
			tree.put ("account", handler.node.ledger.account (transaction, block_a.hashables.source).to_account ());
			tree.put ("amount", handler.node.ledger.amount (transaction, hash).convert_to<std::string> ());
		}
		else
		{
			tree.put ("account", network_params.ledger.genesis_account.to_account ());
			tree.put ("amount", network_params.ledger.genesis_amount.convert_to<std::string> ());
		}
	}
	void change_block (nano::change_block const & block_a)
	{
		if (raw && accounts_filter.empty ())
		{
			tree.put ("type", "change");
			tree.put ("representative", block_a.hashables.representative.to_account ());
			tree.put ("previous", block_a.hashables.previous.to_string ());
		}
	}
	void state_block (nano::state_block const & block_a)
	{
		if (raw)
		{
			tree.put ("type", "state");
			tree.put ("representative", block_a.hashables.representative.to_account ());
			tree.put ("link", block_a.hashables.link.to_string ());
			tree.put ("balance", block_a.hashables.balance.to_string_dec ());
			tree.put ("previous", block_a.hashables.previous.to_string ());
		}
		auto balance (block_a.hashables.balance.number ());
		auto previous_balance (handler.node.ledger.balance (transaction, block_a.hashables.previous));
		if (balance < previous_balance)
		{
			if (should_ignore_account (block_a.hashables.link))
			{
				tree.clear ();
				return;
			}
			if (raw)
			{
				tree.put ("subtype", "send");
			}
			else
			{
				tree.put ("type", "send");
			}
			tree.put ("account", block_a.hashables.link.to_account ());
			tree.put ("amount", (previous_balance - balance).convert_to<std::string> ());
		}
		else
		{
			if (block_a.hashables.link.is_zero ())
			{
				if (raw && accounts_filter.empty ())
				{
					tree.put ("subtype", "change");
				}
			}
			else if (balance == previous_balance && !handler.node.ledger.epoch_link.is_zero () && handler.node.ledger.is_epoch_link (block_a.hashables.link))
			{
				if (raw && accounts_filter.empty ())
				{
					tree.put ("subtype", "epoch");
					tree.put ("account", handler.node.ledger.epoch_signer.to_account ());
				}
			}
			else
			{
				auto account (handler.node.ledger.account (transaction, block_a.hashables.link));
				if (should_ignore_account (account))
				{
					tree.clear ();
					return;
				}
				if (raw)
				{
					tree.put ("subtype", "receive");
				}
				else
				{
					tree.put ("type", "receive");
				}
				tree.put ("account", account.to_account ());
				tree.put ("amount", (balance - previous_balance).convert_to<std::string> ());
			}
		}
	}
	bool should_ignore_account (nano::public_key const & account)
	{
		bool ignore (false);
		if (!accounts_filter.empty ())
		{
			if (std::find (accounts_filter.begin (), accounts_filter.end (), account) == accounts_filter.end ())
			{
				ignore = true;
			}
		}
		return ignore;
	}
	nano::json_handler & handler;
	bool raw;
	nano::transaction & transaction;
	boost::property_tree::ptree & tree;
	nano::block_hash const & hash;
	nano::network_params network_params;
	std::vector<nano::public_key> const & accounts_filter;
};
}

void nano::json_handler::account_history ()
{
	std::vector<nano::public_key> accounts_to_filter;
	const auto accounts_filter_node = request.get_child_optional ("account_filter");
	if (accounts_filter_node.is_initialized ())
	{
		for (auto & a : (*accounts_filter_node))
		{
			auto account (account_impl (a.second.get<std::string> ("")));
			if (!ec)
			{
				accounts_to_filter.push_back (account);
			}
			else
			{
				break;
			}
		}
	}
	nano::account account;
	nano::block_hash hash;
	bool reverse (request.get_optional<bool> ("reverse") == true);
	auto head_str (request.get_optional<std::string> ("head"));
	auto transaction (node.store.tx_begin_read ());
	auto count (count_impl ());
	auto offset (offset_optional_impl (0));
	if (head_str)
	{
		if (!hash.decode_hex (*head_str))
		{
			if (node.store.block_exists (transaction, hash))
			{
				account = node.ledger.account (transaction, hash);
			}
			else
			{
				ec = nano::error_blocks::not_found;
			}
		}
		else
		{
			ec = nano::error_blocks::bad_hash_number;
		}
	}
	else
	{
		account = account_impl ();
		if (!ec)
		{
			if (reverse)
			{
				nano::account_info info;
				if (!node.store.account_get (transaction, account, info))
				{
					hash = info.open_block;
				}
				else
				{
					ec = nano::error_common::account_not_found;
				}
			}
			else
			{
				hash = node.ledger.latest (transaction, account);
			}
		}
	}
	if (!ec)
	{
		boost::property_tree::ptree history;
		bool output_raw (request.get_optional<bool> ("raw") == true);
		response_l.put ("account", account.to_account ());
		nano::block_sideband sideband;
		auto block (node.store.block_get (transaction, hash, &sideband));
		while (block != nullptr && count > 0)
		{
			if (offset > 0)
			{
				--offset;
			}
			else
			{
				boost::property_tree::ptree entry;
				history_visitor visitor (*this, output_raw, transaction, entry, hash, accounts_to_filter);
				block->visit (visitor);
				if (!entry.empty ())
				{
					entry.put ("local_timestamp", std::to_string (sideband.timestamp));
					entry.put ("height", std::to_string (sideband.height));
					entry.put ("hash", hash.to_string ());
					if (output_raw)
					{
						entry.put ("work", nano::to_string_hex (block->block_work ()));
						entry.put ("signature", block->block_signature ().to_string ());
					}
					history.push_back (std::make_pair ("", entry));
					--count;
				}
			}
			hash = reverse ? node.store.block_successor (transaction, hash) : block->previous ();
			block = node.store.block_get (transaction, hash, &sideband);
		}
		response_l.add_child ("history", history);
		if (!hash.is_zero ())
		{
			response_l.put (reverse ? "next" : "previous", hash.to_string ());
		}
	}
	response_errors ();
}

void nano::json_handler::keepalive ()
{
	if (!ec)
	{
		std::string address_text (request.get<std::string> ("address"));
		std::string port_text (request.get<std::string> ("port"));
		uint16_t port;
		if (!nano::parse_port (port_text, port))
		{
			node.keepalive (address_text, port);
			response_l.put ("started", "1");
		}
		else
		{
			ec = nano::error_common::invalid_port;
		}
	}
	response_errors ();
}

void nano::json_handler::key_create ()
{
	nano::keypair pair;
	response_l.put ("private", pair.prv.data.to_string ());
	response_l.put ("public", pair.pub.to_string ());
	response_l.put ("account", pair.pub.to_account ());
	response_errors ();
}

void nano::json_handler::key_expand ()
{
	std::string key_text (request.get<std::string> ("key"));
	nano::uint256_union prv;
	if (!prv.decode_hex (key_text))
	{
		nano::uint256_union pub (nano::pub_key (prv));
		response_l.put ("private", prv.to_string ());
		response_l.put ("public", pub.to_string ());
		response_l.put ("account", pub.to_account ());
	}
	else
	{
		ec = nano::error_common::bad_private_key;
	}
	response_errors ();
}

void nano::json_handler::ledger ()
{
	auto count (count_optional_impl ());
	auto threshold (threshold_optional_impl ());
	if (!ec)
	{
		nano::account start (0);
		boost::optional<std::string> account_text (request.get_optional<std::string> ("account"));
		if (account_text.is_initialized ())
		{
			start = account_impl (account_text.get ());
		}
		uint64_t modified_since (0);
		boost::optional<std::string> modified_since_text (request.get_optional<std::string> ("modified_since"));
		if (modified_since_text.is_initialized ())
		{
			if (decode_unsigned (modified_since_text.get (), modified_since))
			{
				ec = nano::error_rpc::invalid_timestamp;
			}
		}
		const bool sorting = request.get<bool> ("sorting", false);
		const bool representative = request.get<bool> ("representative", false);
		const bool weight = request.get<bool> ("weight", false);
		const bool pending = request.get<bool> ("pending", false);
		boost::property_tree::ptree accounts;
		auto transaction (node.store.tx_begin_read ());
		if (!ec && !sorting) // Simple
		{
			for (auto i (node.store.latest_begin (transaction, start)), n (node.store.latest_end ()); i != n && accounts.size () < count; ++i)
			{
				nano::account_info const & info (i->second);
				if (info.modified >= modified_since && (pending || info.balance.number () >= threshold.number ()))
				{
					nano::account const & account (i->first);
					boost::property_tree::ptree response_a;
					if (pending)
					{
						auto account_pending (node.ledger.account_pending (transaction, account));
						if (info.balance.number () + account_pending < threshold.number ())
						{
							continue;
						}
						response_a.put ("pending", account_pending.convert_to<std::string> ());
					}
					response_a.put ("frontier", info.head.to_string ());
					response_a.put ("open_block", info.open_block.to_string ());
					response_a.put ("representative_block", info.rep_block.to_string ());
					std::string balance;
					nano::uint128_union (info.balance).encode_dec (balance);
					response_a.put ("balance", balance);
					response_a.put ("modified_timestamp", std::to_string (info.modified));
					response_a.put ("block_count", std::to_string (info.block_count));
					if (representative)
					{
						auto block (node.store.block_get (transaction, info.rep_block));
						assert (block != nullptr);
						response_a.put ("representative", block->representative ().to_account ());
					}
					if (weight)
					{
						auto account_weight (node.ledger.weight (transaction, account));
						response_a.put ("weight", account_weight.convert_to<std::string> ());
					}
					accounts.push_back (std::make_pair (account.to_account (), response_a));
				}
			}
		}
		else if (!ec) // Sorting
		{
			std::vector<std::pair<nano::uint128_union, nano::account>> ledger_l;
			for (auto i (node.store.latest_begin (transaction, start)), n (node.store.latest_end ()); i != n; ++i)
			{
				nano::account_info const & info (i->second);
				nano::uint128_union balance (info.balance);
				if (info.modified >= modified_since)
				{
					ledger_l.emplace_back (balance, i->first);
				}
			}
			std::sort (ledger_l.begin (), ledger_l.end ());
			std::reverse (ledger_l.begin (), ledger_l.end ());
			nano::account_info info;
			for (auto i (ledger_l.begin ()), n (ledger_l.end ()); i != n && accounts.size () < count; ++i)
			{
				node.store.account_get (transaction, i->second, info);
				if (pending || info.balance.number () >= threshold.number ())
				{
					nano::account const & account (i->second);
					boost::property_tree::ptree response_a;
					if (pending)
					{
						auto account_pending (node.ledger.account_pending (transaction, account));
						if (info.balance.number () + account_pending < threshold.number ())
						{
							continue;
						}
						response_a.put ("pending", account_pending.convert_to<std::string> ());
					}
					response_a.put ("frontier", info.head.to_string ());
					response_a.put ("open_block", info.open_block.to_string ());
					response_a.put ("representative_block", info.rep_block.to_string ());
					std::string balance;
					(i->first).encode_dec (balance);
					response_a.put ("balance", balance);
					response_a.put ("modified_timestamp", std::to_string (info.modified));
					response_a.put ("block_count", std::to_string (info.block_count));
					if (representative)
					{
						auto block (node.store.block_get (transaction, info.rep_block));
						assert (block != nullptr);
						response_a.put ("representative", block->representative ().to_account ());
					}
					if (weight)
					{
						auto account_weight (node.ledger.weight (transaction, account));
						response_a.put ("weight", account_weight.convert_to<std::string> ());
					}
					accounts.push_back (std::make_pair (account.to_account (), response_a));
				}
			}
		}
		response_l.add_child ("accounts", accounts);
	}
	response_errors ();
}

void nano::json_handler::mnano_from_raw (nano::uint128_t ratio)
{
	auto amount (amount_impl ());
	if (!ec)
	{
		auto result (amount.number () / ratio);
		response_l.put ("amount", result.convert_to<std::string> ());
	}
	response_errors ();
}

void nano::json_handler::mnano_to_raw (nano::uint128_t ratio)
{
	auto amount (amount_impl ());
	if (!ec)
	{
		auto result (amount.number () * ratio);
		if (result > amount.number ())
		{
			response_l.put ("amount", result.convert_to<std::string> ());
		}
		else
		{
			ec = nano::error_common::invalid_amount_big;
		}
	}
	response_errors ();
}

/*
 * @warning This is an internal/diagnostic RPC, do not rely on its interface being stable
 */
void nano::json_handler::node_id ()
{
	if (!ec)
	{
		response_l.put ("private", node.node_id.prv.data.to_string ());
		response_l.put ("public", node.node_id.pub.to_string ());
		response_l.put ("as_account", node.node_id.pub.to_account ());
		response_l.put ("node_id", node.node_id.pub.to_node_id ());
	}
	response_errors ();
}

/*
 * @warning This is an internal/diagnostic RPC, do not rely on its interface being stable
 */
void nano::json_handler::node_id_delete ()
{
	response_l.put ("deprecated", "1");
	response_errors ();
}

void nano::json_handler::password_change ()
{
	auto rpc_l (shared_from_this ());
	node.worker.push_task ([rpc_l]() {
		auto wallet (rpc_l->wallet_impl ());
		if (!rpc_l->ec)
		{
			auto transaction (rpc_l->node.wallets.tx_begin_write ());
			rpc_l->wallet_locked_impl (transaction, wallet);
			if (!rpc_l->ec)
			{
				std::string password_text (rpc_l->request.get<std::string> ("password"));
				bool error (wallet->store.rekey (transaction, password_text));
				rpc_l->response_l.put ("changed", error ? "0" : "1");
				if (!error)
				{
					rpc_l->node.logger.try_log ("Wallet password changed");
				}
			}
		}
		rpc_l->response_errors ();
	});
}

void nano::json_handler::password_enter ()
{
	auto rpc_l (shared_from_this ());
	node.worker.push_task ([rpc_l]() {
		auto wallet (rpc_l->wallet_impl ());
		if (!rpc_l->ec)
		{
			std::string password_text (rpc_l->request.get<std::string> ("password"));
			auto transaction (wallet->wallets.tx_begin_write ());
			auto error (wallet->enter_password (transaction, password_text));
			rpc_l->response_l.put ("valid", error ? "0" : "1");
		}
		rpc_l->response_errors ();
	});
}

void nano::json_handler::password_valid (bool wallet_locked)
{
	auto wallet (wallet_impl ());
	if (!ec)
	{
		auto transaction (node.wallets.tx_begin_read ());
		auto valid (wallet->store.valid_password (transaction));
		if (!wallet_locked)
		{
			response_l.put ("valid", valid ? "1" : "0");
		}
		else
		{
			response_l.put ("locked", valid ? "0" : "1");
		}
	}
	response_errors ();
}

void nano::json_handler::peers ()
{
	boost::property_tree::ptree peers_l;
	const bool peer_details = request.get<bool> ("peer_details", false);
	auto peers_list (node.network.list (std::numeric_limits<size_t>::max ()));
	std::sort (peers_list.begin (), peers_list.end (), [](const auto & lhs, const auto & rhs) {
		return lhs->get_endpoint () < rhs->get_endpoint ();
	});
	for (auto i (peers_list.begin ()), n (peers_list.end ()); i != n; ++i)
	{
		std::stringstream text;
		auto channel (*i);
		text << channel->to_string ();
		if (peer_details)
		{
			boost::property_tree::ptree pending_tree;
			pending_tree.put ("protocol_version", std::to_string (channel->get_network_version ()));
			auto node_id_l (channel->get_node_id_optional ());
			if (node_id_l.is_initialized ())
			{
				pending_tree.put ("node_id", node_id_l.get ().to_node_id ());
			}
			else
			{
				pending_tree.put ("node_id", "");
			}
			pending_tree.put ("type", channel->get_type () == nano::transport::transport_type::tcp ? "tcp" : "udp");
			peers_l.push_back (boost::property_tree::ptree::value_type (text.str (), pending_tree));
		}
		else
		{
			peers_l.push_back (boost::property_tree::ptree::value_type (text.str (), boost::property_tree::ptree (std::to_string (channel->get_network_version ()))));
		}
	}
	response_l.add_child ("peers", peers_l);
	response_errors ();
}

void nano::json_handler::pending ()
{
	auto account (account_impl ());
	auto count (count_optional_impl ());
	auto threshold (threshold_optional_impl ());
	const bool source = request.get<bool> ("source", false);
	const bool min_version = request.get<bool> ("min_version", false);
	const bool include_active = request.get<bool> ("include_active", false);
	const bool include_only_confirmed = request.get<bool> ("include_only_confirmed", false);
	const bool sorting = request.get<bool> ("sorting", false);
	auto simple (threshold.is_zero () && !source && !min_version && !sorting); // if simple, response is a list of hashes
	if (!ec)
	{
		boost::property_tree::ptree peers_l;
		auto transaction (node.store.tx_begin_read ());
		for (auto i (node.store.pending_begin (transaction, nano::pending_key (account, 0))); nano::pending_key (i->first).account == account && peers_l.size () < count; ++i)
		{
			nano::pending_key const & key (i->first);
			if (block_confirmed (node, transaction, key.hash, include_active, include_only_confirmed))
			{
				if (simple)
				{
					boost::property_tree::ptree entry;
					entry.put ("", key.hash.to_string ());
					peers_l.push_back (std::make_pair ("", entry));
				}
				else
				{
					nano::pending_info const & info (i->second);
					if (info.amount.number () >= threshold.number ())
					{
						if (source || min_version)
						{
							boost::property_tree::ptree pending_tree;
							pending_tree.put ("amount", info.amount.number ().convert_to<std::string> ());
							if (source)
							{
								pending_tree.put ("source", info.source.to_account ());
							}
							if (min_version)
							{
								pending_tree.put ("min_version", info.epoch == nano::epoch::epoch_1 ? "1" : "0");
							}
							peers_l.add_child (key.hash.to_string (), pending_tree);
						}
						else
						{
							peers_l.put (key.hash.to_string (), info.amount.number ().convert_to<std::string> ());
						}
					}
				}
			}
		}
		if (sorting && !simple)
		{
			if (source || min_version)
			{
				peers_l.sort ([](const auto & child1, const auto & child2) -> bool {
					return child1.second.template get<nano::uint128_t> ("amount") > child2.second.template get<nano::uint128_t> ("amount");
				});
			}
			else
			{
				peers_l.sort ([](const auto & child1, const auto & child2) -> bool {
					return child1.second.template get<nano::uint128_t> ("") > child2.second.template get<nano::uint128_t> ("");
				});
			}
		}
		response_l.add_child ("blocks", peers_l);
	}
	response_errors ();
}

void nano::json_handler::pending_exists ()
{
	auto hash (hash_impl ());
	const bool include_active = request.get<bool> ("include_active", false);
	const bool include_only_confirmed = request.get<bool> ("include_only_confirmed", false);
	if (!ec)
	{
		auto transaction (node.store.tx_begin_read ());
		auto block (node.store.block_get (transaction, hash));
		if (block != nullptr)
		{
			auto exists (false);
			auto destination (node.ledger.block_destination (transaction, *block));
			if (!destination.is_zero ())
			{
				exists = node.store.pending_exists (transaction, nano::pending_key (destination, hash));
			}
			exists = exists && (block_confirmed (node, transaction, block->hash (), include_active, include_only_confirmed));
			response_l.put ("exists", exists ? "1" : "0");
		}
		else
		{
			ec = nano::error_blocks::not_found;
		}
	}
	response_errors ();
}

void nano::json_handler::payment_begin ()
{
	auto rpc_l (shared_from_this ());
	node.worker.push_task ([rpc_l]() {
		std::string id_text (rpc_l->request.get<std::string> ("wallet"));
		nano::uint256_union id;
		if (!id.decode_hex (id_text))
		{
			auto existing (rpc_l->node.wallets.items.find (id));
			if (existing != rpc_l->node.wallets.items.end ())
			{
				auto transaction (rpc_l->node.wallets.tx_begin_write ());
				std::shared_ptr<nano::wallet> wallet (existing->second);
				if (wallet->store.valid_password (transaction))
				{
					nano::account account (0);
					do
					{
						auto existing (wallet->free_accounts.begin ());
						if (existing != wallet->free_accounts.end ())
						{
							account = *existing;
							wallet->free_accounts.erase (existing);
							if (wallet->store.find (transaction, account) == wallet->store.end ())
							{
								rpc_l->node.logger.always_log (boost::str (boost::format ("Transaction wallet %1% externally modified listing account %2% as free but no longer exists") % id.to_string () % account.to_account ()));
								account.clear ();
							}
							else
							{
								auto block_transaction (rpc_l->node.store.tx_begin_read ());
								if (!rpc_l->node.ledger.account_balance (block_transaction, account).is_zero ())
								{
									rpc_l->node.logger.always_log (boost::str (boost::format ("Skipping account %1% for use as a transaction account: non-zero balance") % account.to_account ()));
									account.clear ();
								}
							}
						}
						else
						{
							account = wallet->deterministic_insert (transaction);
							break;
						}
					} while (account.is_zero ());
					if (!account.is_zero ())
					{
						rpc_l->response_l.put ("deprecated", "1");
						rpc_l->response_l.put ("account", account.to_account ());
					}
					else
					{
						rpc_l->ec = nano::error_rpc::payment_unable_create_account;
					}
				}
				else
				{
					rpc_l->ec = nano::error_common::wallet_locked;
				}
			}
			else
			{
				rpc_l->ec = nano::error_common::wallet_not_found;
			}
		}
		else
		{
			rpc_l->ec = nano::error_common::bad_wallet_number;
		}
		rpc_l->response_errors ();
	});
}

void nano::json_handler::payment_init ()
{
	auto rpc_l (shared_from_this ());
	node.worker.push_task ([rpc_l]() {
		auto wallet (rpc_l->wallet_impl ());
		if (!rpc_l->ec)
		{
			auto transaction (rpc_l->node.wallets.tx_begin_write ());
			if (wallet->store.valid_password (transaction))
			{
				wallet->init_free_accounts (transaction);
				rpc_l->response_l.put ("deprecated", "1");
				rpc_l->response_l.put ("status", "Ready");
			}
			else
			{
				rpc_l->ec = nano::error_common::wallet_locked;
			}
		}
		rpc_l->response_errors ();
	});
}

void nano::json_handler::payment_end ()
{
	auto account (account_impl ());
	auto wallet (wallet_impl ());
	if (!ec)
	{
		auto transaction (node.wallets.tx_begin_read ());
		auto block_transaction (node.store.tx_begin_read ());
		wallet_account_impl (transaction, wallet, account);
		if (!ec)
		{
			if (node.ledger.account_balance (block_transaction, account).is_zero ())
			{
				wallet->free_accounts.insert (account);
				response_l.put ("deprecated", "1");
				response_l.put ("ended", "1");
			}
			else
			{
				ec = nano::error_rpc::payment_account_balance;
			}
		}
	}
	response_errors ();
}

void nano::json_handler::payment_wait ()
{
	std::string timeout_text (request.get<std::string> ("timeout"));
	auto account (account_impl ());
	auto amount (amount_impl ());
	if (!ec)
	{
		uint64_t timeout;
		if (!decode_unsigned (timeout_text, timeout))
		{
			{
				auto observer (std::make_shared<nano::json_payment_observer> (node, response, account, amount));
				observer->start (timeout);
				node.payment_observer_processor.add (account, observer);
			}
			node.payment_observer_processor.observer_action (account);
		}
		else
		{
			ec = nano::error_rpc::bad_timeout;
		}
	}
	if (ec)
	{
		response_errors ();
	}
}

void nano::json_handler::process ()
{
	auto rpc_l (shared_from_this ());
	node.worker.push_task ([rpc_l]() {
		const bool json_block_l = rpc_l->request.get<bool> ("json_block", false);
		const bool watch_work_l = rpc_l->request.get<bool> ("watch_work", true);
		std::shared_ptr<nano::block> block;
		if (json_block_l)
		{
			block = rpc_l->block_json_impl (true);
		}
		else
		{
			block = rpc_l->block_impl (true);
		}

		// State blocks subtype check
		if (!rpc_l->ec && block->type () == nano::block_type::state)
		{
			std::string subtype_text (rpc_l->request.get<std::string> ("subtype", ""));
			if (!subtype_text.empty ())
			{
				std::shared_ptr<nano::state_block> block_state (std::static_pointer_cast<nano::state_block> (block));
				auto transaction (rpc_l->node.store.tx_begin_read ());
				if (!block_state->hashables.previous.is_zero () && !rpc_l->node.store.block_exists (transaction, block_state->hashables.previous))
				{
					rpc_l->ec = nano::error_process::gap_previous;
				}
				else
				{
					auto balance (rpc_l->node.ledger.account_balance (transaction, block_state->hashables.account));
					if (subtype_text == "send")
					{
						if (balance <= block_state->hashables.balance.number ())
						{
							rpc_l->ec = nano::error_rpc::invalid_subtype_balance;
						}
						// Send with previous == 0 fails balance check. No previous != 0 check required
					}
					else if (subtype_text == "receive")
					{
						if (balance > block_state->hashables.balance.number ())
						{
							rpc_l->ec = nano::error_rpc::invalid_subtype_balance;
						}
						// Receive can be point to open block. No previous != 0 check required
					}
					else if (subtype_text == "open")
					{
						if (!block_state->hashables.previous.is_zero ())
						{
							rpc_l->ec = nano::error_rpc::invalid_subtype_previous;
						}
					}
					else if (subtype_text == "change")
					{
						if (balance != block_state->hashables.balance.number ())
						{
							rpc_l->ec = nano::error_rpc::invalid_subtype_balance;
						}
						else if (block_state->hashables.previous.is_zero ())
						{
							rpc_l->ec = nano::error_rpc::invalid_subtype_previous;
						}
					}
					else if (subtype_text == "epoch")
					{
						if (balance != block_state->hashables.balance.number ())
						{
							rpc_l->ec = nano::error_rpc::invalid_subtype_balance;
						}
						else if (!rpc_l->node.ledger.is_epoch_link (block_state->hashables.link))
						{
							rpc_l->ec = nano::error_rpc::invalid_subtype_epoch_link;
						}
					}
					else
					{
						rpc_l->ec = nano::error_rpc::invalid_subtype;
					}
				}
			}
		}
		if (!rpc_l->ec)
		{
			if (!nano::work_validate (*block))
			{
				auto result (rpc_l->node.process_local (block, watch_work_l));
				switch (result.code)
				{
					case nano::process_result::progress:
					{
						rpc_l->response_l.put ("hash", block->hash ().to_string ());
						break;
					}
					case nano::process_result::gap_previous:
					{
						rpc_l->ec = nano::error_process::gap_previous;
						break;
					}
					case nano::process_result::gap_source:
					{
						rpc_l->ec = nano::error_process::gap_source;
						break;
					}
					case nano::process_result::old:
					{
						rpc_l->ec = nano::error_process::old;
						break;
					}
					case nano::process_result::bad_signature:
					{
						rpc_l->ec = nano::error_process::bad_signature;
						break;
					}
					case nano::process_result::negative_spend:
					{
						// TODO once we get RPC versioning, this should be changed to "negative spend"
						rpc_l->ec = nano::error_process::negative_spend;
						break;
					}
					case nano::process_result::balance_mismatch:
					{
						rpc_l->ec = nano::error_process::balance_mismatch;
						break;
					}
					case nano::process_result::unreceivable:
					{
						rpc_l->ec = nano::error_process::unreceivable;
						break;
					}
					case nano::process_result::block_position:
					{
						rpc_l->ec = nano::error_process::block_position;
						break;
					}
					case nano::process_result::fork:
					{
						const bool force = rpc_l->request.get<bool> ("force", false);
						if (force)
						{
							rpc_l->node.active.erase (*block);
							rpc_l->node.block_processor.force (block);
							rpc_l->response_l.put ("hash", block->hash ().to_string ());
						}
						else
						{
							rpc_l->ec = nano::error_process::fork;
						}
						break;
					}
					default:
					{
						rpc_l->ec = nano::error_process::other;
						break;
					}
				}
			}
			else
			{
				rpc_l->ec = nano::error_blocks::work_low;
			}
		}
		rpc_l->response_errors ();
	});
}

void nano::json_handler::receive ()
{
	auto wallet (wallet_impl ());
	auto account (account_impl ());
	auto hash (hash_impl ("block"));
	if (!ec)
	{
		auto transaction (node.wallets.tx_begin_read ());
		wallet_locked_impl (transaction, wallet);
		wallet_account_impl (transaction, wallet, account);
		if (!ec)
		{
			auto block_transaction (node.store.tx_begin_read ());
			auto block (node.store.block_get (block_transaction, hash));
			if (block != nullptr)
			{
				if (node.store.pending_exists (block_transaction, nano::pending_key (account, hash)))
				{
					auto work (work_optional_impl ());
					if (!ec && work)
					{
						nano::account_info info;
						nano::uint256_union head;
						if (!node.store.account_get (block_transaction, account, info))
						{
							head = info.head;
						}
						else
						{
							head = account;
						}
						if (nano::work_validate (head, work))
						{
							ec = nano::error_common::invalid_work;
						}
					}
					if (!ec)
					{
						bool generate_work (work == 0); // Disable work generation if "work" option is provided
						auto response_a (response);
						// clang-format off
						wallet->receive_async(std::move(block), account, node.network_params.ledger.genesis_amount, [response_a](std::shared_ptr<nano::block> block_a) {
							if (block_a != nullptr)
							{
								boost::property_tree::ptree response_l;
								response_l.put("block", block_a->hash().to_string());
								std::stringstream ostream;
								boost::property_tree::write_json(ostream, response_l);
								response_a(ostream.str());
							}
							else
							{
								json_error_response(response_a, "Error generating block");
							}
						},
							work, generate_work);
						// clang-format on
					}
				}
				else
				{
					ec = nano::error_process::unreceivable;
				}
			}
			else
			{
				ec = nano::error_blocks::not_found;
			}
		}
	}
	// Because of receive_async
	if (ec)
	{
		response_errors ();
	}
}

void nano::json_handler::receive_minimum ()
{
	if (!ec)
	{
		response_l.put ("amount", node.config.receive_minimum.to_string_dec ());
	}
	response_errors ();
}

void nano::json_handler::receive_minimum_set ()
{
	auto amount (amount_impl ());
	if (!ec)
	{
		node.config.receive_minimum = amount;
		response_l.put ("success", "");
	}
	response_errors ();
}

void nano::json_handler::representatives ()
{
	auto count (count_optional_impl ());
	if (!ec)
	{
		const bool sorting = request.get<bool> ("sorting", false);
		boost::property_tree::ptree representatives;
		auto rep_amounts = node.ledger.rep_weights.get_rep_amounts ();
		if (!sorting) // Simple
		{
			std::map<nano::account, nano::uint128_t> ordered (rep_amounts.begin (), rep_amounts.end ());
			for (auto & rep_amount : rep_amounts)
			{
				auto const & account (rep_amount.first);
				auto const & amount (rep_amount.second);
				representatives.put (account.to_account (), amount.convert_to<std::string> ());

				if (representatives.size () > count)
				{
					break;
				}
			}
		}
		else // Sorting
		{
			std::vector<std::pair<nano::uint128_t, std::string>> representation;

			for (auto & rep_amount : rep_amounts)
			{
				auto const & account (rep_amount.first);
				auto const & amount (rep_amount.second);
				representation.emplace_back (amount, account.to_account ());
			}
			std::sort (representation.begin (), representation.end ());
			std::reverse (representation.begin (), representation.end ());
			for (auto i (representation.begin ()), n (representation.end ()); i != n && representatives.size () < count; ++i)
			{
				representatives.put (i->second, (i->first).convert_to<std::string> ());
			}
		}
		response_l.add_child ("representatives", representatives);
	}
	response_errors ();
}

void nano::json_handler::representatives_online ()
{
	const auto accounts_node = request.get_child_optional ("accounts");
	const bool weight = request.get<bool> ("weight", false);
	std::vector<nano::public_key> accounts_to_filter;
	if (accounts_node.is_initialized ())
	{
		for (auto & a : (*accounts_node))
		{
			auto account (account_impl (a.second.get<std::string> ("")));
			if (!ec)
			{
				accounts_to_filter.push_back (account);
			}
			else
			{
				break;
			}
		}
	}
	if (!ec)
	{
		boost::property_tree::ptree representatives;
		auto transaction (node.store.tx_begin_read ());
		auto reps (node.online_reps.list ());
		for (auto & i : reps)
		{
			if (accounts_node.is_initialized ())
			{
				if (accounts_to_filter.empty ())
				{
					break;
				}
				auto found_acc = std::find (accounts_to_filter.begin (), accounts_to_filter.end (), i);
				if (found_acc == accounts_to_filter.end ())
				{
					continue;
				}
				else
				{
					accounts_to_filter.erase (found_acc);
				}
			}
			if (weight)
			{
				boost::property_tree::ptree weight_node;
				auto account_weight (node.ledger.weight (transaction, i));
				weight_node.put ("weight", account_weight.convert_to<std::string> ());
				representatives.add_child (i.to_account (), weight_node);
			}
			else
			{
				boost::property_tree::ptree entry;
				entry.put ("", i.to_account ());
				representatives.push_back (std::make_pair ("", entry));
			}
		}
		response_l.add_child ("representatives", representatives);
	}
	response_errors ();
}

void nano::json_handler::republish ()
{
	auto count (count_optional_impl (1024U));
	uint64_t sources (0);
	uint64_t destinations (0);
	boost::optional<std::string> sources_text (request.get_optional<std::string> ("sources"));
	if (!ec && sources_text.is_initialized ())
	{
		if (decode_unsigned (sources_text.get (), sources))
		{
			ec = nano::error_rpc::invalid_sources;
		}
	}
	boost::optional<std::string> destinations_text (request.get_optional<std::string> ("destinations"));
	if (!ec && destinations_text.is_initialized ())
	{
		if (decode_unsigned (destinations_text.get (), destinations))
		{
			ec = nano::error_rpc::invalid_destinations;
		}
	}
	auto hash (hash_impl ());
	if (!ec)
	{
		boost::property_tree::ptree blocks;
		auto transaction (node.store.tx_begin_read ());
		auto block (node.store.block_get (transaction, hash));
		if (block != nullptr)
		{
			std::deque<std::shared_ptr<nano::block>> republish_bundle;
			for (auto i (0); !hash.is_zero () && i < count; ++i)
			{
				block = node.store.block_get (transaction, hash);
				if (sources != 0) // Republish source chain
				{
					nano::block_hash source (node.ledger.block_source (transaction, *block));
					auto block_a (node.store.block_get (transaction, source));
					std::vector<nano::block_hash> hashes;
					while (block_a != nullptr && hashes.size () < sources)
					{
						hashes.push_back (source);
						source = block_a->previous ();
						block_a = node.store.block_get (transaction, source);
					}
					std::reverse (hashes.begin (), hashes.end ());
					for (auto & hash_l : hashes)
					{
						block_a = node.store.block_get (transaction, hash_l);
						republish_bundle.push_back (std::move (block_a));
						boost::property_tree::ptree entry_l;
						entry_l.put ("", hash_l.to_string ());
						blocks.push_back (std::make_pair ("", entry_l));
					}
				}
				republish_bundle.push_back (std::move (block)); // Republish block
				boost::property_tree::ptree entry;
				entry.put ("", hash.to_string ());
				blocks.push_back (std::make_pair ("", entry));
				if (destinations != 0) // Republish destination chain
				{
					auto block_b (node.store.block_get (transaction, hash));
					auto destination (node.ledger.block_destination (transaction, *block_b));
					if (!destination.is_zero ())
					{
						if (!node.store.pending_exists (transaction, nano::pending_key (destination, hash)))
						{
							nano::block_hash previous (node.ledger.latest (transaction, destination));
							auto block_d (node.store.block_get (transaction, previous));
							nano::block_hash source;
							std::vector<nano::block_hash> hashes;
							while (block_d != nullptr && hash != source)
							{
								hashes.push_back (previous);
								source = node.ledger.block_source (transaction, *block_d);
								previous = block_d->previous ();
								block_d = node.store.block_get (transaction, previous);
							}
							std::reverse (hashes.begin (), hashes.end ());
							if (hashes.size () > destinations)
							{
								hashes.resize (destinations);
							}
							for (auto & hash_l : hashes)
							{
								block_d = node.store.block_get (transaction, hash_l);
								republish_bundle.push_back (std::move (block_d));
								boost::property_tree::ptree entry_l;
								entry_l.put ("", hash_l.to_string ());
								blocks.push_back (std::make_pair ("", entry_l));
							}
						}
					}
				}
				hash = node.store.block_successor (transaction, hash);
			}
			node.network.flood_block_batch (std::move (republish_bundle), 25);
			response_l.put ("success", ""); // obsolete
			response_l.add_child ("blocks", blocks);
		}
		else
		{
			ec = nano::error_blocks::not_found;
		}
	}
	response_errors ();
}

void nano::json_handler::search_pending ()
{
	auto wallet (wallet_impl ());
	if (!ec)
	{
		auto error (wallet->search_pending ());
		response_l.put ("started", !error);
	}
	response_errors ();
}

void nano::json_handler::search_pending_all ()
{
	if (!ec)
	{
		node.wallets.search_pending_all ();
		response_l.put ("success", "");
	}
	response_errors ();
}

void nano::json_handler::send ()
{
	auto wallet (wallet_impl ());
	auto amount (amount_impl ());
	// Sending 0 amount is invalid with state blocks
	if (!ec && amount.is_zero ())
	{
		ec = nano::error_common::invalid_amount;
	}
	std::string source_text (request.get<std::string> ("source"));
	auto source (account_impl (source_text, nano::error_rpc::bad_source));
	std::string destination_text (request.get<std::string> ("destination"));
	auto destination (account_impl (destination_text, nano::error_rpc::bad_destination));
	if (!ec)
	{
		auto work (work_optional_impl ());
		nano::uint128_t balance (0);
		if (!ec)
		{
			auto transaction (node.wallets.tx_begin_read ());
			auto block_transaction (node.store.tx_begin_read ());
			if (wallet->store.valid_password (transaction))
			{
				if (wallet->store.find (transaction, source) != wallet->store.end ())
				{
					nano::account_info info;
					if (!node.store.account_get (block_transaction, source, info))
					{
						balance = (info.balance).number ();
					}
					else
					{
						ec = nano::error_common::account_not_found;
					}
					if (!ec && work)
					{
						if (nano::work_validate (info.head, work))
						{
							ec = nano::error_common::invalid_work;
						}
					}
				}
				else
				{
					ec = nano::error_common::account_not_found_wallet;
				}
			}
			else
			{
				ec = nano::error_common::wallet_locked;
			}
		}
		if (!ec)
		{
			bool generate_work (work == 0); // Disable work generation if "work" option is provided
			boost::optional<std::string> send_id (request.get_optional<std::string> ("id"));
			auto response_a (response);
			auto response_data (std::make_shared<boost::property_tree::ptree> (response_l));
			// clang-format off
			wallet->send_async(source, destination, amount.number(), [balance, amount, response_a, response_data](std::shared_ptr<nano::block> block_a) {
				if (block_a != nullptr)
				{
					response_data->put("block", block_a->hash().to_string());
					std::stringstream ostream;
					boost::property_tree::write_json(ostream, *response_data);
					response_a(ostream.str());
				}
				else
				{
					if (balance >= amount.number())
					{
						json_error_response(response_a, "Error generating block");
					}
					else
					{
						std::error_code ec(nano::error_common::insufficient_balance);
						json_error_response(response_a, ec.message());
					}
				}
			},
				work, generate_work, send_id);
			// clang-format on
		}
	}
	// Because of send_async
	if (ec)
	{
		response_errors ();
	}
}

void nano::json_handler::sign ()
{
	const bool json_block_l = request.get<bool> ("json_block", false);
	// Retrieving hash
	nano::block_hash hash (0);
	boost::optional<std::string> hash_text (request.get_optional<std::string> ("hash"));
	if (hash_text.is_initialized ())
	{
		hash = hash_impl ();
	}
	// Retrieving block
	std::shared_ptr<nano::block> block;
	boost::optional<std::string> block_text (request.get_optional<std::string> ("block"));
	if (!ec && block_text.is_initialized ())
	{
		if (json_block_l)
		{
			block = block_json_impl (true);
		}
		else
		{
			block = block_impl (true);
		}
		if (block != nullptr)
		{
			hash = block->hash ();
		}
	}

	// Hash or block are not initialized
	if (!ec && hash.is_zero ())
	{
		ec = nano::error_blocks::invalid_block;
	}
	// Hash is initialized without config permission
	else if (!ec && !hash.is_zero () && block == nullptr && !node_rpc_config.enable_sign_hash)
	{
		ec = nano::error_rpc::sign_hash_disabled;
	}
	if (!ec)
	{
		nano::raw_key prv;
		prv.data.clear ();
		// Retrieving private key from request
		boost::optional<std::string> key_text (request.get_optional<std::string> ("key"));
		if (key_text.is_initialized ())
		{
			if (prv.data.decode_hex (key_text.get ()))
			{
				ec = nano::error_common::bad_private_key;
			}
		}
		else
		{
			// Retrieving private key from wallet
			boost::optional<std::string> account_text (request.get_optional<std::string> ("account"));
			boost::optional<std::string> wallet_text (request.get_optional<std::string> ("wallet"));
			if (wallet_text.is_initialized () && account_text.is_initialized ())
			{
				auto account (account_impl ());
				auto wallet (wallet_impl ());
				if (!ec)
				{
					auto transaction (node.wallets.tx_begin_read ());
					wallet_locked_impl (transaction, wallet);
					wallet_account_impl (transaction, wallet, account);
					if (!ec)
					{
						wallet->store.fetch (transaction, account, prv);
					}
				}
			}
		}
		// Signing
		if (prv.data != 0)
		{
			nano::public_key pub (nano::pub_key (prv.data));
			nano::signature signature (nano::sign_message (prv, pub, hash));
			response_l.put ("signature", signature.to_string ());
			if (block != nullptr)
			{
				block->signature_set (signature);

				if (json_block_l)
				{
					boost::property_tree::ptree block_node_l;
					block->serialize_json (block_node_l);
					response_l.add_child ("block", block_node_l);
				}
				else
				{
					std::string contents;
					block->serialize_json (contents);
					response_l.put ("block", contents);
				}
			}
		}
		else
		{
			ec = nano::error_rpc::block_create_key_required;
		}
	}
	response_errors ();
}

void nano::json_handler::stats ()
{
	auto sink = node.stats.log_sink_json ();
	std::string type (request.get<std::string> ("type", ""));
	bool use_sink = false;
	if (type == "counters")
	{
		node.stats.log_counters (*sink);
		use_sink = true;
	}
	else if (type == "objects")
	{
		construct_json (collect_seq_con_info (node, "node").get (), response_l);
	}
	else if (type == "samples")
	{
		node.stats.log_samples (*sink);
		use_sink = true;
	}
	else
	{
		ec = nano::error_rpc::invalid_missing_type;
	}
	if (!ec && use_sink)
	{
		auto stat_tree_l (*static_cast<boost::property_tree::ptree *> (sink->to_object ()));
		stat_tree_l.put ("stat_duration_seconds", node.stats.last_reset ().count ());
		std::stringstream ostream;
		boost::property_tree::write_json (ostream, stat_tree_l);
		response (ostream.str ());
	}
	else
	{
		response_errors ();
	}
}

void nano::json_handler::stats_clear ()
{
	node.stats.clear ();
	response_l.put ("success", "");
	std::stringstream ostream;
	boost::property_tree::write_json (ostream, response_l);
	response (ostream.str ());
}

void nano::json_handler::stop ()
{
	response_l.put ("success", "");
	response_errors ();
	if (!ec)
	{
		node.stop ();
		stop_callback ();
	}
}

void nano::json_handler::unchecked ()
{
	auto count (count_optional_impl ());
	if (!ec)
	{
		boost::property_tree::ptree unchecked;
		auto transaction (node.store.tx_begin_read ());
		for (auto i (node.store.unchecked_begin (transaction)), n (node.store.unchecked_end ()); i != n && unchecked.size () < count; ++i)
		{
			nano::unchecked_info const & info (i->second);
			std::string contents;
			info.block->serialize_json (contents);
			unchecked.put (info.block->hash ().to_string (), contents);
		}
		response_l.add_child ("blocks", unchecked);
	}
	response_errors ();
}

void nano::json_handler::unchecked_clear ()
{
	auto rpc_l (shared_from_this ());
	node.worker.push_task ([rpc_l]() {
		auto transaction (rpc_l->node.store.tx_begin_write ());
		rpc_l->node.store.unchecked_clear (transaction);
		rpc_l->response_l.put ("success", "");
		rpc_l->response_errors ();
	});
}

void nano::json_handler::unchecked_get ()
{
	const bool json_block_l = request.get<bool> ("json_block", false);
	auto hash (hash_impl ());
	if (!ec)
	{
		auto transaction (node.store.tx_begin_read ());
		for (auto i (node.store.unchecked_begin (transaction)), n (node.store.unchecked_end ()); i != n; ++i)
		{
			nano::unchecked_key const & key (i->first);
			if (key.hash == hash)
			{
				nano::unchecked_info const & info (i->second);
				response_l.put ("modified_timestamp", std::to_string (info.modified));

				if (json_block_l)
				{
					boost::property_tree::ptree block_node_l;
					info.block->serialize_json (block_node_l);
					response_l.add_child ("contents", block_node_l);
				}
				else
				{
					std::string contents;
					info.block->serialize_json (contents);
					response_l.put ("contents", contents);
				}
				break;
			}
		}
		if (response_l.empty ())
		{
			ec = nano::error_blocks::not_found;
		}
	}
	response_errors ();
}

void nano::json_handler::unchecked_keys ()
{
	const bool json_block_l = request.get<bool> ("json_block", false);
	auto count (count_optional_impl ());
	nano::uint256_union key (0);
	boost::optional<std::string> hash_text (request.get_optional<std::string> ("key"));
	if (!ec && hash_text.is_initialized ())
	{
		if (key.decode_hex (hash_text.get ()))
		{
			ec = nano::error_rpc::bad_key;
		}
	}
	if (!ec)
	{
		boost::property_tree::ptree unchecked;
		auto transaction (node.store.tx_begin_read ());
		for (auto i (node.store.unchecked_begin (transaction, nano::unchecked_key (key, 0))), n (node.store.unchecked_end ()); i != n && unchecked.size () < count; ++i)
		{
			boost::property_tree::ptree entry;
			nano::unchecked_info const & info (i->second);
			entry.put ("key", nano::block_hash (i->first.key ()).to_string ());
			entry.put ("hash", info.block->hash ().to_string ());
			entry.put ("modified_timestamp", std::to_string (info.modified));
			if (json_block_l)
			{
				boost::property_tree::ptree block_node_l;
				info.block->serialize_json (block_node_l);
				entry.add_child ("contents", block_node_l);
			}
			else
			{
				std::string contents;
				info.block->serialize_json (contents);
				entry.put ("contents", contents);
			}
			unchecked.push_back (std::make_pair ("", entry));
		}
		response_l.add_child ("unchecked", unchecked);
	}
	response_errors ();
}

void nano::json_handler::unopened ()
{
	auto count (count_optional_impl ());
	auto threshold (threshold_optional_impl ());
	nano::account start (1); // exclude burn account by default
	boost::optional<std::string> account_text (request.get_optional<std::string> ("account"));
	if (account_text.is_initialized ())
	{
		start = account_impl (account_text.get ());
	}
	if (!ec)
	{
		auto transaction (node.store.tx_begin_read ());
		auto iterator (node.store.pending_begin (transaction, nano::pending_key (start, 0)));
		auto end (node.store.pending_end ());
		nano::account current_account (start);
		nano::uint128_t current_account_sum{ 0 };
		boost::property_tree::ptree accounts;
		while (iterator != end && accounts.size () < count)
		{
			nano::pending_key key (iterator->first);
			nano::account account (key.account);
			nano::pending_info info (iterator->second);
			if (node.store.account_exists (transaction, account))
			{
				if (account.number () == std::numeric_limits<nano::uint256_t>::max ())
				{
					break;
				}
				// Skip existing accounts
				iterator = node.store.pending_begin (transaction, nano::pending_key (account.number () + 1, 0));
			}
			else
			{
				if (account != current_account)
				{
					if (current_account_sum > 0)
					{
						if (current_account_sum >= threshold.number ())
						{
							accounts.put (current_account.to_account (), current_account_sum.convert_to<std::string> ());
						}
						current_account_sum = 0;
					}
					current_account = account;
				}
				current_account_sum += info.amount.number ();
				++iterator;
			}
		}
		// last one after iterator reaches end
		if (accounts.size () < count && current_account_sum > 0 && current_account_sum >= threshold.number ())
		{
			accounts.put (current_account.to_account (), current_account_sum.convert_to<std::string> ());
		}
		response_l.add_child ("accounts", accounts);
	}
	response_errors ();
}

void nano::json_handler::uptime ()
{
	response_l.put ("seconds", std::chrono::duration_cast<std::chrono::seconds> (std::chrono::steady_clock::now () - node.startup_time).count ());
	response_errors ();
}

void nano::json_handler::version ()
{
	response_l.put ("rpc_version", "1");
	response_l.put ("store_version", std::to_string (node.store_version ()));
	response_l.put ("protocol_version", std::to_string (node.network_params.protocol.protocol_version));
	response_l.put ("node_vendor", boost::str (boost::format ("Nano %1%") % NANO_VERSION_STRING));
	response_l.put ("network", node.network_params.network.get_current_network_as_string ());
	response_l.put ("network_identifier", nano::genesis ().hash ().to_string ());
	response_l.put ("build_info", BUILD_INFO);
	response_errors ();
}

void nano::json_handler::validate_account_number ()
{
	auto account (account_impl ());
	response_l.put ("valid", ec ? "0" : "1");
	ec = std::error_code (); // error is just invalid account
	response_errors ();
}

void nano::json_handler::wallet_add ()
{
	auto rpc_l (shared_from_this ());
	node.worker.push_task ([rpc_l]() {
		auto wallet (rpc_l->wallet_impl ());
		if (!rpc_l->ec)
		{
			std::string key_text (rpc_l->request.get<std::string> ("key"));
			nano::raw_key key;
			if (!key.data.decode_hex (key_text))
			{
				const bool generate_work = rpc_l->request.get<bool> ("work", true);
				auto pub (wallet->insert_adhoc (key, generate_work));
				if (!pub.is_zero ())
				{
					rpc_l->response_l.put ("account", pub.to_account ());
				}
				else
				{
					rpc_l->ec = nano::error_common::wallet_locked;
				}
			}
			else
			{
				rpc_l->ec = nano::error_common::bad_private_key;
			}
		}
		rpc_l->response_errors ();
	});
}

void nano::json_handler::wallet_add_watch ()
{
	auto rpc_l (shared_from_this ());
	node.worker.push_task ([rpc_l]() {
		auto wallet (rpc_l->wallet_impl ());
		if (!rpc_l->ec)
		{
			auto transaction (rpc_l->node.wallets.tx_begin_write ());
			if (wallet->store.valid_password (transaction))
			{
				for (auto & accounts : rpc_l->request.get_child ("accounts"))
				{
					auto account (rpc_l->account_impl (accounts.second.data ()));
					if (!rpc_l->ec)
					{
						wallet->insert_watch (transaction, account);
					}
				}
				rpc_l->response_l.put ("success", "");
			}
			else
			{
				rpc_l->ec = nano::error_common::wallet_locked;
			}
		}
		rpc_l->response_errors ();
	});
}

void nano::json_handler::wallet_info ()
{
	auto wallet (wallet_impl ());
	if (!ec)
	{
		nano::uint128_t balance (0);
		nano::uint128_t pending (0);
		uint64_t count (0);
		uint64_t deterministic_count (0);
		uint64_t adhoc_count (0);
		auto transaction (node.wallets.tx_begin_read ());
		auto block_transaction (node.store.tx_begin_read ());
		for (auto i (wallet->store.begin (transaction)), n (wallet->store.end ()); i != n; ++i)
		{
			nano::account const & account (i->first);
			balance = balance + node.ledger.account_balance (block_transaction, account);
			pending = pending + node.ledger.account_pending (block_transaction, account);
			nano::key_type key_type (wallet->store.key_type (i->second));
			if (key_type == nano::key_type::deterministic)
			{
				deterministic_count++;
			}
			else if (key_type == nano::key_type::adhoc)
			{
				adhoc_count++;
			}
			count++;
		}
		uint32_t deterministic_index (wallet->store.deterministic_index_get (transaction));
		response_l.put ("balance", balance.convert_to<std::string> ());
		response_l.put ("pending", pending.convert_to<std::string> ());
		response_l.put ("accounts_count", std::to_string (count));
		response_l.put ("deterministic_count", std::to_string (deterministic_count));
		response_l.put ("adhoc_count", std::to_string (adhoc_count));
		response_l.put ("deterministic_index", std::to_string (deterministic_index));
	}
	response_errors ();
}

void nano::json_handler::wallet_balances ()
{
	auto wallet (wallet_impl ());
	auto threshold (threshold_optional_impl ());
	if (!ec)
	{
		boost::property_tree::ptree balances;
		auto transaction (node.wallets.tx_begin_read ());
		auto block_transaction (node.store.tx_begin_read ());
		for (auto i (wallet->store.begin (transaction)), n (wallet->store.end ()); i != n; ++i)
		{
			nano::account const & account (i->first);
			nano::uint128_t balance = node.ledger.account_balance (block_transaction, account);
			if (balance >= threshold.number ())
			{
				boost::property_tree::ptree entry;
				nano::uint128_t pending = node.ledger.account_pending (block_transaction, account);
				entry.put ("balance", balance.convert_to<std::string> ());
				entry.put ("pending", pending.convert_to<std::string> ());
				balances.push_back (std::make_pair (account.to_account (), entry));
			}
		}
		response_l.add_child ("balances", balances);
	}
	response_errors ();
}

void nano::json_handler::wallet_change_seed ()
{
	auto rpc_l (shared_from_this ());
	node.worker.push_task ([rpc_l]() {
		auto wallet (rpc_l->wallet_impl ());
		if (!rpc_l->ec)
		{
			std::string seed_text (rpc_l->request.get<std::string> ("seed"));
			nano::raw_key seed;
			if (!seed.data.decode_hex (seed_text))
			{
				auto count (static_cast<uint32_t> (rpc_l->count_optional_impl (0)));
				auto transaction (rpc_l->node.wallets.tx_begin_write ());
				if (wallet->store.valid_password (transaction))
				{
					nano::public_key account (wallet->change_seed (transaction, seed, count));
					rpc_l->response_l.put ("success", "");
					rpc_l->response_l.put ("last_restored_account", account.to_account ());
					auto index (wallet->store.deterministic_index_get (transaction));
					assert (index > 0);
					rpc_l->response_l.put ("restored_count", std::to_string (index));
				}
				else
				{
					rpc_l->ec = nano::error_common::wallet_locked;
				}
			}
			else
			{
				rpc_l->ec = nano::error_common::bad_seed;
			}
		}
		rpc_l->response_errors ();
	});
}

void nano::json_handler::wallet_contains ()
{
	auto account (account_impl ());
	auto wallet (wallet_impl ());
	if (!ec)
	{
		auto transaction (node.wallets.tx_begin_read ());
		auto exists (wallet->store.find (transaction, account) != wallet->store.end ());
		response_l.put ("exists", exists ? "1" : "0");
	}
	response_errors ();
}

void nano::json_handler::wallet_create ()
{
	auto rpc_l (shared_from_this ());
	node.worker.push_task ([rpc_l]() {
		nano::raw_key seed;
		auto seed_text (rpc_l->request.get_optional<std::string> ("seed"));
		if (seed_text.is_initialized () && seed.data.decode_hex (seed_text.get ()))
		{
			rpc_l->ec = nano::error_common::bad_seed;
		}
		if (!rpc_l->ec)
		{
			nano::keypair wallet_id;
			auto wallet (rpc_l->node.wallets.create (wallet_id.pub));
			auto existing (rpc_l->node.wallets.items.find (wallet_id.pub));
			if (existing != rpc_l->node.wallets.items.end ())
			{
				rpc_l->response_l.put ("wallet", wallet_id.pub.to_string ());
			}
			else
			{
				rpc_l->ec = nano::error_common::wallet_lmdb_max_dbs;
			}
			if (!rpc_l->ec && seed_text.is_initialized ())
			{
				auto transaction (rpc_l->node.wallets.tx_begin_write ());
				nano::public_key account (wallet->change_seed (transaction, seed));
				rpc_l->response_l.put ("last_restored_account", account.to_account ());
				auto index (wallet->store.deterministic_index_get (transaction));
				assert (index > 0);
				rpc_l->response_l.put ("restored_count", std::to_string (index));
			}
		}
		rpc_l->response_errors ();
	});
}

void nano::json_handler::wallet_destroy ()
{
	auto rpc_l (shared_from_this ());
	node.worker.push_task ([rpc_l]() {
		std::string wallet_text (rpc_l->request.get<std::string> ("wallet"));
		nano::uint256_union wallet;
		if (!wallet.decode_hex (wallet_text))
		{
			auto existing (rpc_l->node.wallets.items.find (wallet));
			if (existing != rpc_l->node.wallets.items.end ())
			{
				rpc_l->node.wallets.destroy (wallet);
				bool destroyed (rpc_l->node.wallets.items.find (wallet) == rpc_l->node.wallets.items.end ());
				rpc_l->response_l.put ("destroyed", destroyed ? "1" : "0");
			}
			else
			{
				rpc_l->ec = nano::error_common::wallet_not_found;
			}
		}
		else
		{
			rpc_l->ec = nano::error_common::bad_wallet_number;
		}
		rpc_l->response_errors ();
	});
}

void nano::json_handler::wallet_export ()
{
	auto wallet (wallet_impl ());
	if (!ec)
	{
		auto transaction (node.wallets.tx_begin_read ());
		std::string json;
		wallet->store.serialize_json (transaction, json);
		response_l.put ("json", json);
	}
	response_errors ();
}

void nano::json_handler::wallet_frontiers ()
{
	auto wallet (wallet_impl ());
	if (!ec)
	{
		boost::property_tree::ptree frontiers;
		auto transaction (node.wallets.tx_begin_read ());
		auto block_transaction (node.store.tx_begin_read ());
		for (auto i (wallet->store.begin (transaction)), n (wallet->store.end ()); i != n; ++i)
		{
			nano::account const & account (i->first);
			auto latest (node.ledger.latest (block_transaction, account));
			if (!latest.is_zero ())
			{
				frontiers.put (account.to_account (), latest.to_string ());
			}
		}
		response_l.add_child ("frontiers", frontiers);
	}
	response_errors ();
}

void nano::json_handler::wallet_history ()
{
	uint64_t modified_since (1);
	boost::optional<std::string> modified_since_text (request.get_optional<std::string> ("modified_since"));
	if (modified_since_text.is_initialized ())
	{
		if (decode_unsigned (modified_since_text.get (), modified_since))
		{
			ec = nano::error_rpc::invalid_timestamp;
		}
	}
	auto wallet (wallet_impl ());
	if (!ec)
	{
		std::multimap<uint64_t, boost::property_tree::ptree, std::greater<uint64_t>> entries;
		auto transaction (node.wallets.tx_begin_read ());
		auto block_transaction (node.store.tx_begin_read ());
		for (auto i (wallet->store.begin (transaction)), n (wallet->store.end ()); i != n; ++i)
		{
			nano::account const & account (i->first);
			nano::account_info info;
			if (!node.store.account_get (block_transaction, account, info))
			{
				auto timestamp (info.modified);
				auto hash (info.head);
				while (timestamp >= modified_since && !hash.is_zero ())
				{
					nano::block_sideband sideband;
					auto block (node.store.block_get (block_transaction, hash, &sideband));
					timestamp = sideband.timestamp;
					if (block != nullptr && timestamp >= modified_since)
					{
						boost::property_tree::ptree entry;
						std::vector<nano::public_key> no_filter;
						history_visitor visitor (*this, false, block_transaction, entry, hash, no_filter);
						block->visit (visitor);
						if (!entry.empty ())
						{
							entry.put ("block_account", account.to_account ());
							entry.put ("hash", hash.to_string ());
							entry.put ("local_timestamp", std::to_string (timestamp));
							entries.insert (std::make_pair (timestamp, entry));
						}
						hash = block->previous ();
					}
					else
					{
						hash.clear ();
					}
				}
			}
		}
		boost::property_tree::ptree history;
		for (auto i (entries.begin ()), n (entries.end ()); i != n; ++i)
		{
			history.push_back (std::make_pair ("", i->second));
		}
		response_l.add_child ("history", history);
	}
	response_errors ();
}

void nano::json_handler::wallet_key_valid ()
{
	auto wallet (wallet_impl ());
	if (!ec)
	{
		auto transaction (node.wallets.tx_begin_read ());
		auto valid (wallet->store.valid_password (transaction));
		response_l.put ("valid", valid ? "1" : "0");
	}
	response_errors ();
}

void nano::json_handler::wallet_ledger ()
{
	const bool representative = request.get<bool> ("representative", false);
	const bool weight = request.get<bool> ("weight", false);
	const bool pending = request.get<bool> ("pending", false);
	uint64_t modified_since (0);
	boost::optional<std::string> modified_since_text (request.get_optional<std::string> ("modified_since"));
	if (modified_since_text.is_initialized ())
	{
		modified_since = strtoul (modified_since_text.get ().c_str (), NULL, 10);
	}
	auto wallet (wallet_impl ());
	if (!ec)
	{
		boost::property_tree::ptree accounts;
		auto transaction (node.wallets.tx_begin_read ());
		auto block_transaction (node.store.tx_begin_read ());
		for (auto i (wallet->store.begin (transaction)), n (wallet->store.end ()); i != n; ++i)
		{
			nano::account const & account (i->first);
			nano::account_info info;
			if (!node.store.account_get (block_transaction, account, info))
			{
				if (info.modified >= modified_since)
				{
					boost::property_tree::ptree entry;
					entry.put ("frontier", info.head.to_string ());
					entry.put ("open_block", info.open_block.to_string ());
					entry.put ("representative_block", info.rep_block.to_string ());
					std::string balance;
					nano::uint128_union (info.balance).encode_dec (balance);
					entry.put ("balance", balance);
					entry.put ("modified_timestamp", std::to_string (info.modified));
					entry.put ("block_count", std::to_string (info.block_count));
					if (representative)
					{
						auto block (node.store.block_get (block_transaction, info.rep_block));
						assert (block != nullptr);
						entry.put ("representative", block->representative ().to_account ());
					}
					if (weight)
					{
						auto account_weight (node.ledger.weight (block_transaction, account));
						entry.put ("weight", account_weight.convert_to<std::string> ());
					}
					if (pending)
					{
						auto account_pending (node.ledger.account_pending (block_transaction, account));
						entry.put ("pending", account_pending.convert_to<std::string> ());
					}
					accounts.push_back (std::make_pair (account.to_account (), entry));
				}
			}
		}
		response_l.add_child ("accounts", accounts);
	}
	response_errors ();
}

void nano::json_handler::wallet_lock ()
{
	auto wallet (wallet_impl ());
	if (!ec)
	{
		nano::raw_key empty;
		empty.data.clear ();
		wallet->store.password.value_set (empty);
		response_l.put ("locked", "1");
		node.logger.try_log ("Wallet locked");
	}
	response_errors ();
}

void nano::json_handler::wallet_pending ()
{
	auto wallet (wallet_impl ());
	auto count (count_optional_impl ());
	auto threshold (threshold_optional_impl ());
	const bool source = request.get<bool> ("source", false);
	const bool min_version = request.get<bool> ("min_version", false);
	const bool include_active = request.get<bool> ("include_active", false);
	const bool include_only_confirmed = request.get<bool> ("include_only_confirmed", false);
	if (!ec)
	{
		boost::property_tree::ptree pending;
		auto transaction (node.wallets.tx_begin_read ());
		auto block_transaction (node.store.tx_begin_read ());
		for (auto i (wallet->store.begin (transaction)), n (wallet->store.end ()); i != n; ++i)
		{
			nano::account const & account (i->first);
			boost::property_tree::ptree peers_l;
			for (auto ii (node.store.pending_begin (block_transaction, nano::pending_key (account, 0))); nano::pending_key (ii->first).account == account && peers_l.size () < count; ++ii)
			{
				nano::pending_key key (ii->first);
				if (block_confirmed (node, block_transaction, key.hash, include_active, include_only_confirmed))
				{
					if (threshold.is_zero () && !source)
					{
						boost::property_tree::ptree entry;
						entry.put ("", key.hash.to_string ());
						peers_l.push_back (std::make_pair ("", entry));
					}
					else
					{
						nano::pending_info info (ii->second);
						if (info.amount.number () >= threshold.number ())
						{
							if (source || min_version)
							{
								boost::property_tree::ptree pending_tree;
								pending_tree.put ("amount", info.amount.number ().convert_to<std::string> ());
								if (source)
								{
									pending_tree.put ("source", info.source.to_account ());
								}
								if (min_version)
								{
									pending_tree.put ("min_version", info.epoch == nano::epoch::epoch_1 ? "1" : "0");
								}
								peers_l.add_child (key.hash.to_string (), pending_tree);
							}
							else
							{
								peers_l.put (key.hash.to_string (), info.amount.number ().convert_to<std::string> ());
							}
						}
					}
				}
			}
			if (!peers_l.empty ())
			{
				pending.add_child (account.to_account (), peers_l);
			}
		}
		response_l.add_child ("blocks", pending);
	}
	response_errors ();
}

void nano::json_handler::wallet_representative ()
{
	auto wallet (wallet_impl ());
	if (!ec)
	{
		auto transaction (node.wallets.tx_begin_read ());
		response_l.put ("representative", wallet->store.representative (transaction).to_account ());
	}
	response_errors ();
}

void nano::json_handler::wallet_representative_set ()
{
	auto rpc_l (shared_from_this ());
	node.worker.push_task ([rpc_l]() {
		auto wallet (rpc_l->wallet_impl ());
		std::string representative_text (rpc_l->request.get<std::string> ("representative"));
		auto representative (rpc_l->account_impl (representative_text, nano::error_rpc::bad_representative_number));
		if (!rpc_l->ec)
		{
			bool update_existing_accounts (rpc_l->request.get<bool> ("update_existing_accounts", false));
			{
				auto transaction (rpc_l->node.wallets.tx_begin_write ());
				if (wallet->store.valid_password (transaction) || !update_existing_accounts)
				{
					wallet->store.representative_set (transaction, representative);
					rpc_l->response_l.put ("set", "1");
				}
				else
				{
					rpc_l->ec = nano::error_common::wallet_locked;
				}
			}
			// Change representative for all wallet accounts
			if (!rpc_l->ec && update_existing_accounts)
			{
				std::vector<nano::account> accounts;
				{
					auto transaction (rpc_l->node.wallets.tx_begin_read ());
					auto block_transaction (rpc_l->node.store.tx_begin_read ());
					for (auto i (wallet->store.begin (transaction)), n (wallet->store.end ()); i != n; ++i)
					{
						nano::account const & account (i->first);
						nano::account_info info;
						if (!rpc_l->node.store.account_get (block_transaction, account, info))
						{
							auto block (rpc_l->node.store.block_get (block_transaction, info.rep_block));
							assert (block != nullptr);
							if (block->representative () != representative)
							{
								accounts.push_back (account);
							}
						}
					}
				}
				for (auto & account : accounts)
				{
					// clang-format off
					wallet->change_async(account, representative, [](std::shared_ptr<nano::block>) {}, 0, false);
					// clang-format on
				}
			}
		}
		rpc_l->response_errors ();
	});
}

void nano::json_handler::wallet_republish ()
{
	auto wallet (wallet_impl ());
	auto count (count_impl ());
	if (!ec)
	{
		boost::property_tree::ptree blocks;
		std::deque<std::shared_ptr<nano::block>> republish_bundle;
		auto transaction (node.wallets.tx_begin_read ());
		auto block_transaction (node.store.tx_begin_read ());
		for (auto i (wallet->store.begin (transaction)), n (wallet->store.end ()); i != n; ++i)
		{
			nano::account const & account (i->first);
			auto latest (node.ledger.latest (block_transaction, account));
			std::shared_ptr<nano::block> block;
			std::vector<nano::block_hash> hashes;
			while (!latest.is_zero () && hashes.size () < count)
			{
				hashes.push_back (latest);
				block = node.store.block_get (block_transaction, latest);
				latest = block->previous ();
			}
			std::reverse (hashes.begin (), hashes.end ());
			for (auto & hash : hashes)
			{
				block = node.store.block_get (block_transaction, hash);
				republish_bundle.push_back (std::move (block));
				boost::property_tree::ptree entry;
				entry.put ("", hash.to_string ());
				blocks.push_back (std::make_pair ("", entry));
			}
		}
		node.network.flood_block_batch (std::move (republish_bundle), 25);
		response_l.add_child ("blocks", blocks);
	}
	response_errors ();
}

void nano::json_handler::wallet_seed ()
{
	auto wallet (wallet_impl ());
	if (!ec)
	{
		auto transaction (node.wallets.tx_begin_read ());
		if (wallet->store.valid_password (transaction))
		{
			nano::raw_key seed;
			wallet->store.seed (seed, transaction);
			response_l.put ("seed", seed.data.to_string ());
		}
		else
		{
			ec = nano::error_common::wallet_locked;
		}
	}
	response_errors ();
}

void nano::json_handler::wallet_work_get ()
{
	auto wallet (wallet_impl ());
	if (!ec)
	{
		boost::property_tree::ptree works;
		auto transaction (node.wallets.tx_begin_read ());
		for (auto i (wallet->store.begin (transaction)), n (wallet->store.end ()); i != n; ++i)
		{
			nano::account const & account (i->first);
			uint64_t work (0);
			auto error_work (wallet->store.work_get (transaction, account, work));
			(void)error_work;
			works.put (account.to_account (), nano::to_string_hex (work));
		}
		response_l.add_child ("works", works);
	}
	response_errors ();
}

void nano::json_handler::work_generate ()
{
	auto hash (hash_impl ());
	auto difficulty (difficulty_optional_impl ());
	multiplier_optional_impl (difficulty);
	if (!ec && (difficulty > node.config.max_work_generate_difficulty || difficulty < node.network_params.network.publish_threshold))
	{
		ec = nano::error_rpc::difficulty_limit;
	}
	if (!ec)
	{
		bool use_peers (request.get_optional<bool> ("use_peers") == true);
		auto rpc_l (shared_from_this ());
		auto callback = [rpc_l, hash, this](boost::optional<uint64_t> const & work_a) {
			if (work_a)
			{
				uint64_t work (work_a.value ());
				boost::property_tree::ptree response_l;
				response_l.put ("work", nano::to_string_hex (work));
				std::stringstream ostream;
				uint64_t result_difficulty;
				nano::work_validate (hash, work, &result_difficulty);
				response_l.put ("difficulty", nano::to_string_hex (result_difficulty));
				auto result_multiplier = nano::difficulty::to_multiplier (result_difficulty, this->node.network_params.network.publish_threshold);
				response_l.put ("multiplier", nano::to_string (result_multiplier));
				boost::property_tree::write_json (ostream, response_l);
				rpc_l->response (ostream.str ());
			}
			else
			{
				json_error_response (rpc_l->response, "Cancelled");
			}
		};
		if (!use_peers)
		{
			node.work.generate (hash, callback, difficulty);
		}
		else
		{
			node.work_generate (hash, callback, difficulty);
		}
	}
	// Because of callback
	if (ec)
	{
		response_errors ();
	}
}

void nano::json_handler::work_cancel ()
{
	auto hash (hash_impl ());
	if (!ec)
	{
		node.work.cancel (hash);
	}
	response_errors ();
}

void nano::json_handler::work_get ()
{
	auto wallet (wallet_impl ());
	auto account (account_impl ());
	if (!ec)
	{
		auto transaction (node.wallets.tx_begin_read ());
		wallet_account_impl (transaction, wallet, account);
		if (!ec)
		{
			uint64_t work (0);
			auto error_work (wallet->store.work_get (transaction, account, work));
			(void)error_work;
			response_l.put ("work", nano::to_string_hex (work));
		}
	}
	response_errors ();
}

void nano::json_handler::work_set ()
{
	auto rpc_l (shared_from_this ());
	node.worker.push_task ([rpc_l]() {
		auto wallet (rpc_l->wallet_impl ());
		auto account (rpc_l->account_impl ());
		auto work (rpc_l->work_optional_impl ());
		if (!rpc_l->ec)
		{
			auto transaction (rpc_l->node.wallets.tx_begin_write ());
			rpc_l->wallet_account_impl (transaction, wallet, account);
			if (!rpc_l->ec)
			{
				wallet->store.work_put (transaction, account, work);
				rpc_l->response_l.put ("success", "");
			}
		}
		rpc_l->response_errors ();
	});
}

void nano::json_handler::work_validate ()
{
	auto hash (hash_impl ());
	auto work (work_optional_impl ());
	auto difficulty (difficulty_optional_impl ());
	multiplier_optional_impl (difficulty);
	if (!ec)
	{
		uint64_t result_difficulty (0);
		nano::work_validate (hash, work, &result_difficulty);
		response_l.put ("valid", (result_difficulty >= difficulty) ? "1" : "0");
		response_l.put ("difficulty", nano::to_string_hex (result_difficulty));
		auto result_multiplier = nano::difficulty::to_multiplier (result_difficulty, node.network_params.network.publish_threshold);
		response_l.put ("multiplier", nano::to_string (result_multiplier));
	}
	response_errors ();
}

void nano::json_handler::work_peer_add ()
{
	std::string address_text = request.get<std::string> ("address");
	std::string port_text = request.get<std::string> ("port");
	uint16_t port;
	if (!nano::parse_port (port_text, port))
	{
		node.config.work_peers.push_back (std::make_pair (address_text, port));
		response_l.put ("success", "");
	}
	else
	{
		ec = nano::error_common::invalid_port;
	}
	response_errors ();
}

void nano::json_handler::work_peers ()
{
	boost::property_tree::ptree work_peers_l;
	for (auto i (node.config.work_peers.begin ()), n (node.config.work_peers.end ()); i != n; ++i)
	{
		boost::property_tree::ptree entry;
		entry.put ("", boost::str (boost::format ("%1%:%2%") % i->first % i->second));
		work_peers_l.push_back (std::make_pair ("", entry));
	}
	response_l.add_child ("work_peers", work_peers_l);
	response_errors ();
}

void nano::json_handler::work_peers_clear ()
{
	node.config.work_peers.clear ();
	response_l.put ("success", "");
	response_errors ();
}

namespace
{
void construct_json (nano::seq_con_info_component * component, boost::property_tree::ptree & parent)
{
	// We are a leaf node, print name and exit
	if (!component->is_composite ())
	{
		auto & leaf_info = static_cast<nano::seq_con_info_leaf *> (component)->get_info ();
		boost::property_tree::ptree child;
		child.put ("count", leaf_info.count);
		child.put ("size", leaf_info.count * leaf_info.sizeof_element);
		parent.add_child (leaf_info.name, child);
		return;
	}

	auto composite = static_cast<nano::seq_con_info_composite *> (component);

	boost::property_tree::ptree current;
	for (auto & child : composite->get_children ())
	{
		construct_json (child.get (), current);
	}

	parent.add_child (composite->get_name (), current);
}

// Any RPC handlers which require no arguments (excl default arguments) should go here.
// This is to prevent large if/else chains which compilers can have limits for (MSVC for instance has 128).
ipc_json_handler_no_arg_func_map create_ipc_json_handler_no_arg_func_map ()
{
	ipc_json_handler_no_arg_func_map no_arg_funcs;
	no_arg_funcs.emplace ("account_balance", &nano::json_handler::account_balance);
	no_arg_funcs.emplace ("account_block_count", &nano::json_handler::account_block_count);
	no_arg_funcs.emplace ("account_count", &nano::json_handler::account_count);
	no_arg_funcs.emplace ("account_create", &nano::json_handler::account_create);
	no_arg_funcs.emplace ("account_get", &nano::json_handler::account_get);
	no_arg_funcs.emplace ("account_history", &nano::json_handler::account_history);
	no_arg_funcs.emplace ("account_info", &nano::json_handler::account_info);
	no_arg_funcs.emplace ("account_key", &nano::json_handler::account_key);
	no_arg_funcs.emplace ("account_list", &nano::json_handler::account_list);
	no_arg_funcs.emplace ("account_move", &nano::json_handler::account_move);
	no_arg_funcs.emplace ("account_remove", &nano::json_handler::account_remove);
	no_arg_funcs.emplace ("account_representative", &nano::json_handler::account_representative);
	no_arg_funcs.emplace ("account_representative_set", &nano::json_handler::account_representative_set);
	no_arg_funcs.emplace ("account_weight", &nano::json_handler::account_weight);
	no_arg_funcs.emplace ("accounts_balances", &nano::json_handler::accounts_balances);
	no_arg_funcs.emplace ("accounts_create", &nano::json_handler::accounts_create);
	no_arg_funcs.emplace ("accounts_frontiers", &nano::json_handler::accounts_frontiers);
	no_arg_funcs.emplace ("accounts_pending", &nano::json_handler::accounts_pending);
	no_arg_funcs.emplace ("active_difficulty", &nano::json_handler::active_difficulty);
	no_arg_funcs.emplace ("available_supply", &nano::json_handler::available_supply);
	no_arg_funcs.emplace ("block_info", &nano::json_handler::block_info);
	no_arg_funcs.emplace ("block", &nano::json_handler::block_info);
	no_arg_funcs.emplace ("block_confirm", &nano::json_handler::block_confirm);
	no_arg_funcs.emplace ("blocks", &nano::json_handler::blocks);
	no_arg_funcs.emplace ("blocks_info", &nano::json_handler::blocks_info);
	no_arg_funcs.emplace ("block_account", &nano::json_handler::block_account);
	no_arg_funcs.emplace ("block_count", &nano::json_handler::block_count);
	no_arg_funcs.emplace ("block_count_type", &nano::json_handler::block_count_type);
	no_arg_funcs.emplace ("block_create", &nano::json_handler::block_create);
	no_arg_funcs.emplace ("block_hash", &nano::json_handler::block_hash);
	no_arg_funcs.emplace ("bootstrap", &nano::json_handler::bootstrap);
	no_arg_funcs.emplace ("bootstrap_any", &nano::json_handler::bootstrap_any);
	no_arg_funcs.emplace ("bootstrap_lazy", &nano::json_handler::bootstrap_lazy);
	no_arg_funcs.emplace ("bootstrap_status", &nano::json_handler::bootstrap_status);
	no_arg_funcs.emplace ("confirmation_active", &nano::json_handler::confirmation_active);
	no_arg_funcs.emplace ("confirmation_height_currently_processing", &nano::json_handler::confirmation_height_currently_processing);
	no_arg_funcs.emplace ("confirmation_history", &nano::json_handler::confirmation_history);
	no_arg_funcs.emplace ("confirmation_info", &nano::json_handler::confirmation_info);
	no_arg_funcs.emplace ("confirmation_quorum", &nano::json_handler::confirmation_quorum);
	no_arg_funcs.emplace ("database_txn_tracker", &nano::json_handler::database_txn_tracker);
	no_arg_funcs.emplace ("delegators", &nano::json_handler::delegators);
	no_arg_funcs.emplace ("delegators_count", &nano::json_handler::delegators_count);
	no_arg_funcs.emplace ("deterministic_key", &nano::json_handler::deterministic_key);
	no_arg_funcs.emplace ("frontiers", &nano::json_handler::frontiers);
	no_arg_funcs.emplace ("frontier_count", &nano::json_handler::account_count);
	no_arg_funcs.emplace ("keepalive", &nano::json_handler::keepalive);
	no_arg_funcs.emplace ("key_create", &nano::json_handler::key_create);
	no_arg_funcs.emplace ("key_expand", &nano::json_handler::key_expand);
	no_arg_funcs.emplace ("ledger", &nano::json_handler::ledger);
	no_arg_funcs.emplace ("node_id", &nano::json_handler::node_id);
	no_arg_funcs.emplace ("node_id_delete", &nano::json_handler::node_id_delete);
	no_arg_funcs.emplace ("password_change", &nano::json_handler::password_change);
	no_arg_funcs.emplace ("password_enter", &nano::json_handler::password_enter);
	no_arg_funcs.emplace ("wallet_unlock", &nano::json_handler::password_enter);
	no_arg_funcs.emplace ("payment_begin", &nano::json_handler::payment_begin);
	no_arg_funcs.emplace ("payment_init", &nano::json_handler::payment_init);
	no_arg_funcs.emplace ("payment_end", &nano::json_handler::payment_end);
	no_arg_funcs.emplace ("payment_wait", &nano::json_handler::payment_wait);
	no_arg_funcs.emplace ("peers", &nano::json_handler::peers);
	no_arg_funcs.emplace ("pending", &nano::json_handler::pending);
	no_arg_funcs.emplace ("pending_exists", &nano::json_handler::pending_exists);
	no_arg_funcs.emplace ("process", &nano::json_handler::process);
	no_arg_funcs.emplace ("receive", &nano::json_handler::receive);
	no_arg_funcs.emplace ("receive_minimum", &nano::json_handler::receive_minimum);
	no_arg_funcs.emplace ("receive_minimum_set", &nano::json_handler::receive_minimum_set);
	no_arg_funcs.emplace ("representatives", &nano::json_handler::representatives);
	no_arg_funcs.emplace ("representatives_online", &nano::json_handler::representatives_online);
	no_arg_funcs.emplace ("republish", &nano::json_handler::republish);
	no_arg_funcs.emplace ("search_pending", &nano::json_handler::search_pending);
	no_arg_funcs.emplace ("search_pending_all", &nano::json_handler::search_pending_all);
	no_arg_funcs.emplace ("send", &nano::json_handler::send);
	no_arg_funcs.emplace ("sign", &nano::json_handler::sign);
	no_arg_funcs.emplace ("stats", &nano::json_handler::stats);
	no_arg_funcs.emplace ("stats_clear", &nano::json_handler::stats_clear);
	no_arg_funcs.emplace ("stop", &nano::json_handler::stop);
	no_arg_funcs.emplace ("unchecked", &nano::json_handler::unchecked);
	no_arg_funcs.emplace ("unchecked_clear", &nano::json_handler::unchecked_clear);
	no_arg_funcs.emplace ("unchecked_get", &nano::json_handler::unchecked_get);
	no_arg_funcs.emplace ("unchecked_keys", &nano::json_handler::unchecked_keys);
	no_arg_funcs.emplace ("unopened", &nano::json_handler::unopened);
	no_arg_funcs.emplace ("uptime", &nano::json_handler::uptime);
	no_arg_funcs.emplace ("validate_account_number", &nano::json_handler::validate_account_number);
	no_arg_funcs.emplace ("version", &nano::json_handler::version);
	no_arg_funcs.emplace ("wallet_add", &nano::json_handler::wallet_add);
	no_arg_funcs.emplace ("wallet_add_watch", &nano::json_handler::wallet_add_watch);
	no_arg_funcs.emplace ("wallet_balances", &nano::json_handler::wallet_balances);
	no_arg_funcs.emplace ("wallet_change_seed", &nano::json_handler::wallet_change_seed);
	no_arg_funcs.emplace ("wallet_contains", &nano::json_handler::wallet_contains);
	no_arg_funcs.emplace ("wallet_create", &nano::json_handler::wallet_create);
	no_arg_funcs.emplace ("wallet_destroy", &nano::json_handler::wallet_destroy);
	no_arg_funcs.emplace ("wallet_export", &nano::json_handler::wallet_export);
	no_arg_funcs.emplace ("wallet_frontiers", &nano::json_handler::wallet_frontiers);
	no_arg_funcs.emplace ("wallet_history", &nano::json_handler::wallet_history);
	no_arg_funcs.emplace ("wallet_info", &nano::json_handler::wallet_info);
	no_arg_funcs.emplace ("wallet_balance_total", &nano::json_handler::wallet_info);
	no_arg_funcs.emplace ("wallet_key_valid", &nano::json_handler::wallet_key_valid);
	no_arg_funcs.emplace ("wallet_ledger", &nano::json_handler::wallet_ledger);
	no_arg_funcs.emplace ("wallet_lock", &nano::json_handler::wallet_lock);
	no_arg_funcs.emplace ("wallet_pending", &nano::json_handler::wallet_pending);
	no_arg_funcs.emplace ("wallet_representative", &nano::json_handler::wallet_representative);
	no_arg_funcs.emplace ("wallet_representative_set", &nano::json_handler::wallet_representative_set);
	no_arg_funcs.emplace ("wallet_republish", &nano::json_handler::wallet_republish);
	no_arg_funcs.emplace ("wallet_work_get", &nano::json_handler::wallet_work_get);
	no_arg_funcs.emplace ("work_generate", &nano::json_handler::work_generate);
	no_arg_funcs.emplace ("work_cancel", &nano::json_handler::work_cancel);
	no_arg_funcs.emplace ("work_get", &nano::json_handler::work_get);
	no_arg_funcs.emplace ("work_set", &nano::json_handler::work_set);
	no_arg_funcs.emplace ("work_validate", &nano::json_handler::work_validate);
	no_arg_funcs.emplace ("work_peer_add", &nano::json_handler::work_peer_add);
	no_arg_funcs.emplace ("work_peers", &nano::json_handler::work_peers);
	no_arg_funcs.emplace ("work_peers_clear", &nano::json_handler::work_peers_clear);
	return no_arg_funcs;
}

/** Due to the asynchronous nature of updating confirmation heights, it can also be necessary to check active roots */
bool block_confirmed (nano::node & node, nano::transaction & transaction, nano::block_hash const & hash, bool include_active, bool include_only_confirmed)
{
	bool is_confirmed = false;
	if (include_active && !include_only_confirmed)
	{
		is_confirmed = true;
	}
	// Check whether the confirmation height is set
	else if (node.block_confirmed_or_being_confirmed (transaction, hash))
	{
		is_confirmed = true;
	}
	// This just checks it's not currently undergoing an active transaction
	else if (!include_only_confirmed)
	{
		auto block (node.store.block_get (transaction, hash));
		is_confirmed = (block != nullptr && !node.active.active (*block));
	}

	return is_confirmed;
}
}
