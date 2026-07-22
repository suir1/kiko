#include "web/web.hpp"
#include "web/web_assets.hpp"

#include <cassert>
#include <iostream>
#include <string>

int main() {
  using namespace kiko;

  const auto token_a = generate_web_token();
  const auto token_b = generate_web_token();
  assert(token_a.size() == 48);
  assert(token_b.size() == 48);
  assert(token_a != token_b);
  const auto html = std::string(web_index_html());
  assert(html.find("Notepad") != std::string::npos);
  assert(html.find("Start notepad") != std::string::npos);
  assert(html.find("Custom code host") != std::string::npos);
  assert(html.find("note-custom-host") != std::string::npos);
  assert(html.find("noteRole = code && !qs('note-custom-host').checked ? 'join' : 'host'") != std::string::npos);
  assert(html.find("Copy code") != std::string::npos);
  assert(html.find("Copy note") != std::string::npos);
  assert(html.find("New note") != std::string::npos);
  assert(html.find("note-pads") != std::string::npos);
  assert(html.find("showNoteQr") != std::string::npos);
  assert(html.find("/api/qr") != std::string::npos);
  assert(html.find("/api/note/pad/create") != std::string::npos);
  assert(html.find("/api/note/pad/select") != std::string::npos);
  assert(html.find("renderNotePads") != std::string::npos);
  assert(html.find("QR contains the note text directly") != std::string::npos);
  assert(html.find("Host notepad") == std::string::npos);
  assert(html.find("Join notepad") == std::string::npos);
  assert(html.find("note-role") == std::string::npos);
  assert(html.find("/api/note/start") != std::string::npos);
  assert(html.find("/api/note/update") != std::string::npos);
  assert(html.find("noteEditGeneration") != std::string::npos);
  assert(html.find("generation === noteEditGeneration") != std::string::npos);
  assert(html.find("kiko.web.path.") != std::string::npos);
  assert(html.find("kiko.web.browser.") != std::string::npos);
  assert(html.find("send-pick-file") != std::string::npos);
  assert(html.find("send-pick-folder") != std::string::npos);
  assert(html.find("send-browse-paths") != std::string::npos);
  assert(html.find("recv-pick-folder") != std::string::npos);
  assert(html.find("/api/fs/pick") != std::string::npos);
  assert(html.find("send-device-file") != std::string::npos);
  assert(html.find("/api/upload/start") != std::string::npos);
  assert(html.find("browser_file_picker") != std::string::npos);
  assert(html.find("terminalActivity") != std::string::npos);
  assert(html.find("Canceling...") != std::string::npos);
  std::cout << "web_test ok\n";
  return 0;
}
