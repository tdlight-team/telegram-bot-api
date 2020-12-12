//
// Copyright Luckydonald (tdlight-telegram-bot-api+code@luckydonald.de) 2020
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "telegram-bot-api/Query.h"

#include "td/db/TQueue.h"

#include "td/net/HttpOutboundConnection.h"
#include "td/net/HttpQuery.h"
#include "td/net/SslStream.h"

#include "td/actor/actor.h"
#include "td/actor/PromiseFuture.h"

#include "td/utils/common.h"
#include "td/utils/Container.h"
#include "td/utils/FloodControlFast.h"
#include "td/utils/HttpUrl.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/List.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/port/SocketFd.h"
#include "td/utils/Slice.h"
#include "td/utils/Status.h"
#include "td/utils/VectorQueue.h"
#include "td/utils/utf8.h"

#include <atomic>
#include <functional>
#include <memory>
#include <set>
#include <tuple>
#include <unordered_map>

namespace telegram_bot_api {

class JsonStatsSize : public td::Jsonable {
 public:
  JsonStatsSize(td::uint64 size) : size_(size) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("bytes", td::JsonLong(size_));

    // Now basically td::format::as_size(...), but without need for an StringBuilder.
    struct NamedValue {
      const char *name;
      td::uint64 value;
    };

    static constexpr NamedValue sizes[] = {{"B", 1}, {"KB", 1 << 10}, {"MB", 1 << 20}, {"GB", 1 << 30}};
    static constexpr size_t sizes_n = sizeof(sizes) / sizeof(NamedValue);

    size_t i = 0;
    while (i + 1 < sizes_n && size_ > 10 * sizes[i + 1].value) {
      i++;
    }
    object("human_readable", td::to_string(size_ / sizes[i].value) + sizes[i].name);
  }

 private:
  const td::uint64 size_;
};

class JsonStatsMem : public td::Jsonable {
 public:
  JsonStatsMem(const td::MemStat *mem_stat) : mem_stat_(mem_stat) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("resident_size", JsonStatsSize(mem_stat_->resident_size_));
    object("resident_size_peak", JsonStatsSize(mem_stat_->resident_size_peak_));
    object("virtual_size", JsonStatsSize(mem_stat_->virtual_size_));
    object("virtual_size_peak", JsonStatsSize(mem_stat_->virtual_size_peak_));
  }

 private:
  const td::MemStat *mem_stat_;
};


class JsonStatsCpuItem : public td::Jsonable {
 public:
  JsonStatsCpuItem()
      : total_cpu_("unset"), user_cpu_("unset"), system_cpu_("unset") {
  }
  JsonStatsCpuItem(const td::string total_cpu, const td::string user_cpu, const td::string system_cpu)
  : total_cpu_(total_cpu), user_cpu_(user_cpu), system_cpu_(system_cpu) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    // Maybe needs td::JsonString(td::Slice(...)) instead of just a string?
    object("total_cpu", total_cpu_);
    object("user_cpu", user_cpu_);
    object("system_cpu", system_cpu_);
  }

  td::string total_cpu_;
  td::string user_cpu_;
  td::string system_cpu_;
 private:
};

class JsonStatsCpu : public td::Jsonable {
 public:
  JsonStatsCpu(const td::vector<td::vector<StatItem>> *cpu_stats) : cpu_stats_(cpu_stats) {
  }
  void store(td::JsonValueScope *scope) const {
    auto array = scope->enter_array();
    for (const td::vector<StatItem> &stats : *cpu_stats_) {
      auto item = JsonStatsCpuItem();
      for (const auto& stat : stats) {
        if (stat.key_ == "total_cpu") {
          item.total_cpu_ = stat.value_;
        } else if (stat.key_ == "user_cpu") {
          item.user_cpu_ = stat.value_;
        } else if (stat.key_ == "system_cpu") {
          item.system_cpu_ = stat.value_;
        } else {
          ::td::detail::process_check_error(("key '" + stat.key_ + "' must be one of ['total_cpu', 'user_cpu', 'system_cpu']").c_str(), __FILE__, __LINE__);
        }
        array << item;
      }
    }
  }

 private:
  const td::vector<td::vector<StatItem>> *cpu_stats_;
};

class JsonStatsBot : public td::Jsonable {
 public:
  JsonStatsBot(const std::pair<td::int64, td::uint64> *score_id_pair) : score_id_pair_(score_id_pair) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("score", td::JsonLong(score_id_pair_->first));
    object("internal_id", td::JsonLong(score_id_pair_->second));
  }

 protected:
  const std::pair<td::int64, td::uint64> *score_id_pair_;
};

class JsonStatsBotAdvanced : public JsonStatsBot {
 public:
  JsonStatsBotAdvanced(
      const std::pair<td::int64, td::uint64> *score_id_pair,
      const ServerBotInfo *bot,
      const bool hide_sensible_data
    ) :
      JsonStatsBot(score_id_pair), bot_(bot), hide_sensible_data_(hide_sensible_data) {
  }
  void store(td::JsonValueScope *scope) const {
    auto object = scope->enter_object();
    object("id", td::JsonRaw(bot_->id_));
    //object("uptime", now - bot_->start_time_);
    object("score", td::JsonLong(score_id_pair_->first));
    object("internal_id", td::JsonLong(score_id_pair_->second));
    if (!hide_sensible_data_) {
      object("token", bot_->token_);
    }
    object("username", bot_->username_);
    td::CSlice url = bot_->webhook_;
    object("webhook_set", td::JsonBool(!url.empty()));
    if (!hide_sensible_data_) {
      if (td::check_utf8(url)) {
        object("webhook_url", url);
      } else {
        object("webhook_url", td::JsonRawString(url));
      }
    }

    object("has_custom_certificate", td::JsonBool(bot_->has_webhook_certificate_));
    object("head_update_id", td::JsonInt(bot_->head_update_id_));
    object("tail_update_id", td::JsonInt(bot_->tail_update_id_));
    object("pending_update_count", td::narrow_cast<td::int32>(bot_->pending_update_count_));
    object("webhook_max_connections", td::JsonInt(bot_->webhook_max_connections_));
  }
 private:
  const ServerBotInfo *bot_;
  const bool hide_sensible_data_;
};


class JsonStatsBots : public td::Jsonable {
 public:
  JsonStatsBots(const td::vector<JsonStatsBotAdvanced> *bots, bool no_metadata) : bots_(bots), no_metadata_(no_metadata) {
  }
  void store(td::JsonValueScope *scope) const {
    auto array = scope->enter_array();
    if (no_metadata_) {
      for (const JsonStatsBotAdvanced &bot: *bots_) {
        array << static_cast<const JsonStatsBot&>(bot);
      }
    } else {
      for (const JsonStatsBotAdvanced &bot: *bots_) {
        array << bot;
      }
    }
  }
 private:
  const td::vector<JsonStatsBotAdvanced> *bots_;
  bool no_metadata_;
};

}  // namespace telegram_bot_api
