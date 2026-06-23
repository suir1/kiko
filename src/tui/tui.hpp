#pragma once

#include "common.hpp"
#include "transfer/transfer.hpp"

namespace kiko {

// FTXUI front-ends. These run the same kiko_core transfer logic on a worker
// thread while rendering live progress. When stdin is not a TTY they fall back
// to the plain CLI reporter so pipelines and tests still work.
int run_tui_send(const SendConfig& config);
int run_tui_recv(const RecvConfig& config);

// Interactive launcher: pick send/receive and fill in parameters.
int run_tui_menu(const Endpoint& default_relay);

}  // namespace kiko
