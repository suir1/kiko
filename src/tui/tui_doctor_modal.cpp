#include "tui_doctor_modal.hpp"

#include "diagnostics/doctor.hpp"
#include "tui_transfer_view.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <mutex>
#include <thread>

namespace kiko {

void show_doctor_modal(const Endpoint& relay, const std::optional<std::string>& relay_pass, bool udp_probe) {
  using namespace ftxui;

  DoctorOptions opts;
  opts.relay = relay;
  opts.relay_pass = relay_pass;
  opts.udp_probe = udp_probe;

  std::mutex mutex;
  std::string status = "running network check...";
  std::string body;
  bool failed = false;

  auto screen = ScreenInteractive::Fullscreen();
  std::thread worker([&] {
    try {
      const DoctorReport report = run_doctor(opts);
      std::lock_guard<std::mutex> lock(mutex);
      body = report.diagnosis;
      if (body.empty()) body = "check finished (no issues reported)";
      status = "done";
    } catch (const std::exception& e) {
      std::lock_guard<std::mutex> lock(mutex);
      failed = true;
      body = e.what();
      status = "failed";
    }
    screen.PostEvent(Event::Custom);
  });

  auto close_button = Button("Close", [&] { screen.Exit(); });

  auto renderer = Renderer(close_button, [&] {
    std::lock_guard<std::mutex> lock(mutex);
    Elements rows;
    rows.push_back(text("kiko network check") | bold | hcenter);
    rows.push_back(hbox({text("relay: "), text(relay.to_string())}));
    rows.push_back(hbox({text("status: "), text(status) | color(failed ? Color::Red : Color::GreenLight)}));
    rows.push_back(separator());
    for (const auto& line : split_lines(body.empty() ? status : body)) {
      rows.push_back(text(line));
    }
    rows.push_back(separator());
    rows.push_back(close_button->Render() | hcenter);
    return vbox(std::move(rows)) | border;
  });

  auto with_events = CatchEvent(renderer, [&](Event event) {
    if (event == Event::Escape) {
      screen.Exit();
      return true;
    }
    return false;
  });

  screen.Loop(with_events);
  worker.join();
}

}  // namespace kiko
