#include <graphene/chain/database.hpp>
#include <graphene/chain/account_object.hpp>
#include <graphene/chain/asset_object.hpp>
#include <graphene/chain/betting_market_object.hpp>

namespace graphene { namespace chain {

void database::cancel_bet( const bet_object& bet, bool create_virtual_op )
{
   asset amount_to_refund = bet.amount_to_bet;
   amount_to_refund += bet.amount_reserved_for_fees;
   //TODO: update global statistics
   adjust_balance(bet.bettor_id, amount_to_refund); //return unmatched stake
   //TODO: do special fee accounting as required
   if (create_virtual_op)
      push_applied_operation(bet_canceled_operation(bet.bettor_id, bet.id,
                                                    bet.amount_to_bet,
                                                    bet.amount_reserved_for_fees));
   remove(bet);
}

void database::cancel_all_unmatched_bets_on_betting_market(const betting_market_object& betting_market)
{
   auto& bet_index = get_index_type<bet_object_index>().indices().get<by_betting_market>();
   for (const bet_object& bet : bet_index)
      cancel_bet(bet, true);
}

void database::resolve_betting_market(const betting_market_object& betting_market, 
                                      betting_market_resolution_type resolution)
{
   cancel_all_unmatched_bets_on_betting_market(betting_market);

   auto& index = get_index_type<betting_market_position_index>().indices().get<by_bettor_betting_market>();
   auto position_itr = index.lower_bound(std::make_tuple(betting_market.id));
   while (position_itr != index.end() &&
          position_itr->betting_market_id == betting_market.id)
   {
      const betting_market_position_object& position = *position_itr;
      ++position_itr;

      share_type payout_amount = 0;
      switch (resolution)
      {
      case betting_market_resolution_type::win:
         payout_amount += position.pay_if_payout_condition;
         payout_amount += position.pay_if_not_canceled;
         break;
      case betting_market_resolution_type::not_win:
         payout_amount += position.pay_if_not_payout_condition;
         payout_amount += position.pay_if_not_canceled;
         break;
      case betting_market_resolution_type::cancel:
         payout_amount += position.pay_if_canceled;
         break;
      }

      adjust_balance(position.bettor_id, asset(payout_amount, betting_market.asset_id));
      //TODO: pay the fees to the correct (dividend-distribution) account
      adjust_balance(account_id_type(), asset(position.fees_collected, betting_market.asset_id));
      //TODO: generate a virtual op to notify the bettor that they won or lost

      remove(position);
   }
   remove(betting_market);
}

bool maybe_cull_small_bet( database& db, const bet_object& bet_object_to_cull )
{
   /**
    *  There are times when this bet can't be matched (for example, it's now laying a 2:1 bet for
    *  1 satoshi, so it could only be matched by half a satoshi).  Remove these bets from
    *  the books.
    */

   if( bet_object_to_cull.get_matching_amount() == 0 )
   {
      ilog("applied epsilon logic");
      db.cancel_bet(bet_object_to_cull);
      return true;
   }
   return false;
}

share_type adjust_betting_position(database& db, account_id_type bettor_id, betting_market_id_type betting_market_id, bet_type back_or_lay, share_type bet_amount, share_type fees_collected)
{ try {
   assert(bet_amount >= 0);
   
   share_type guaranteed_winnings_returned = 0;

   if (bet_amount == 0)
      return guaranteed_winnings_returned;

   auto& index = db.get_index_type<betting_market_position_index>().indices().get<by_bettor_betting_market>();
   auto itr = index.find(boost::make_tuple(bettor_id, betting_market_id));
   if (itr == index.end())
   {
      db.create<betting_market_position_object>([&](betting_market_position_object& position) {
		 position.bettor_id = bettor_id;
		 position.betting_market_id = betting_market_id;
         position.pay_if_payout_condition = back_or_lay == bet_type::back ? bet_amount : 0;
         position.pay_if_not_payout_condition = back_or_lay == bet_type::lay ? bet_amount : 0;
         position.pay_if_canceled = bet_amount;
         position.pay_if_not_canceled = 0;
         position.fees_collected = fees_collected;
         // this should not be reducible
      });
   } else {
      db.modify(*itr, [&](betting_market_position_object& position) {
		 assert(position.bettor_id == bettor_id);
		 assert(position.betting_market_id == betting_market_id);
         position.pay_if_payout_condition += back_or_lay == bet_type::back ? bet_amount : 0;
         position.pay_if_not_payout_condition += back_or_lay == bet_type::lay ? bet_amount : 0;
         position.pay_if_canceled += bet_amount;
         position.fees_collected += fees_collected;

         guaranteed_winnings_returned = position.reduce();
      });
   }
   return guaranteed_winnings_returned;
} FC_CAPTURE_AND_RETHROW((bettor_id)(betting_market_id)(bet_amount)) }


bool bet_was_matched(database& db, const bet_object& bet, share_type amount_bet, bet_multiplier_type actual_multiplier, bool cull_if_small)
{
   // calculate the percentage fee paid
   fc::uint128_t percentage_fee_128 = bet.amount_reserved_for_fees.value;
   percentage_fee_128 *= amount_bet.value;
   percentage_fee_128 += bet.amount_to_bet.amount.value - 1;
   percentage_fee_128 /= bet.amount_to_bet.amount.value;
   share_type fee_paid = percentage_fee_128.to_uint64();

   // record their bet, modifying their position, and return any winnings
   share_type guaranteed_winnings_returned = adjust_betting_position(db, bet.bettor_id, bet.betting_market_id, 
                                                                     bet.back_or_lay, amount_bet, fee_paid);
   db.adjust_balance(bet.bettor_id, asset(guaranteed_winnings_returned, bet.amount_to_bet.asset_id));

   // generate a virtual "match" op
   asset asset_amount_bet(amount_bet, bet.amount_to_bet.asset_id);
   db.push_applied_operation(bet_matched_operation(bet.bettor_id, bet.id,
                                                   asset_amount_bet,
                                                   fee_paid,
                                                   actual_multiplier,
                                                   guaranteed_winnings_returned));

   // update the bet on the books
   if (asset_amount_bet == bet.amount_to_bet)
   {
      db.remove(bet);
      return true;
   }
   else
   {
      db.modify(bet, [&](bet_object& bet_obj) {
         bet_obj.amount_to_bet -= asset_amount_bet;
         bet_obj.amount_reserved_for_fees -= fee_paid;
      });
      if (cull_if_small)
         return maybe_cull_small_bet(db, bet);
      return false;
   }
}

/**
 *  Matches the two orders,
 *
 *  @return a bit field indicating which orders were filled (and thus removed)
 *
 *  0 - no orders were matched
 *  1 - bid was filled
 *  2 - ask was filled
 *  3 - both were filled
 */
int match_bet(database& db, const bet_object& taker_bet, const bet_object& maker_bet )
{
   assert(taker_bet.amount_to_bet.asset_id == maker_bet.amount_to_bet.asset_id);
   assert(taker_bet.amount_to_bet.amount > 0 && maker_bet.amount_to_bet.amount > 0);
   assert(taker_bet.backer_multiplier >= maker_bet.backer_multiplier);
   assert(taker_bet.back_or_lay != maker_bet.back_or_lay);

   int result = 0;
   share_type maximum_amount_to_match = taker_bet.get_matching_amount();

   if (maximum_amount_to_match <= maker_bet.amount_to_bet.amount)
   {
      // we will consume the entire taker bet
      result |= bet_was_matched(db, taker_bet, taker_bet.amount_to_bet.amount, maker_bet.backer_multiplier, true);
      result |= bet_was_matched(db, maker_bet, maximum_amount_to_match, maker_bet.backer_multiplier, true) << 1;
   }
   else
   {
      // we will consume the entire maker bet.  Figure out how much of the taker bet we can fill.
      share_type taker_amount = maker_bet.get_matching_amount();
      share_type maker_amount = bet_object::get_matching_amount(taker_amount, maker_bet.backer_multiplier, taker_bet.back_or_lay);

      result |= bet_was_matched(db, taker_bet, taker_amount, maker_bet.backer_multiplier, true);
      result |= bet_was_matched(db, maker_bet, maker_amount, maker_bet.backer_multiplier, true) << 1;
   }

   assert(result != 0);
   return result;
}

bool database::place_bet(const bet_object& new_bet_object)
{
   bet_id_type bet_id = new_bet_object.id;
   const asset_object& bet_asset = get(new_bet_object.amount_to_bet.asset_id);

   const auto& bet_odds_idx = get_index_type<bet_object_index>().indices().get<by_odds>();

   // TODO: it should be possible to simply check the NEXT/PREV iterator after new_order_object to
   // determine whether or not this order has "changed the book" in a way that requires us to
   // check orders. For now I just lookup the lower bound and check for equality... this is log(n) vs
   // constant time check. Potential optimization.

   bet_type bet_type_to_match = new_bet_object.back_or_lay == bet_type::back ? bet_type::lay : bet_type::back;
   auto book_itr = bet_odds_idx.lower_bound(std::make_tuple(new_bet_object.betting_market_id, bet_type_to_match));
   auto book_end = bet_odds_idx.upper_bound(std::make_tuple(new_bet_object.betting_market_id, bet_type_to_match, new_bet_object.backer_multiplier));

   int orders_matched_flags = 0;
   bool finished = false;
   while (!finished && book_itr != book_end)
   {
      auto old_book_itr = book_itr;
      ++book_itr;
      // match returns 2 when only the old order was fully filled. In this case, we keep matching; otherwise, we stop.

      orders_matched_flags = match_bet(*this, new_bet_object, *old_book_itr);
      finished = orders_matched_flags != 2;
   }

   return (orders_matched_flags & 1) != 0;
}

} }
